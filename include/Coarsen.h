/*
 *    Copyright (C) 2010 Imperial College London and others.
 *    
 *    Please see the AUTHORS file in the main source directory for a full list
 *    of copyright holders.
 *
 *    Gerard Gorman
 *    Applied Modelling and Computation Group
 *    Department of Earth Science and Engineering
 *    Imperial College London
 *
 *    amcgsoftware@imperial.ac.uk
 *    
 *    This library is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation,
 *    version 2.1 of the License.
 *
 *    This library is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with this library; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *    USA
 */

#ifndef COARSEN_H
#define COARSEN_H

#include <algorithm>
#include <set>
#include <vector>

#include "ElementProperty.h"
#include "zoltan_colour.h"
#include "Mesh.h"

/*! \brief Performs mesh coarsening.
 *
 */

template<typename real_t, typename index_t> class Coarsen{
 public:
  /// Default constructor.
  Coarsen(Mesh<real_t, index_t> &mesh, Surface<real_t, index_t> &surface){
    _mesh = &mesh;
    _surface = &surface;

    ndims = _mesh->get_number_dimensions();
    nloc = (ndims==2)?3:4;
    snloc = (ndims==2)?2:3;

    property = NULL;
    size_t NElements = _mesh->get_number_elements();
    for(size_t i=0;i<NElements;i++){
      const int *n=_mesh->get_element(i);
      if(n[0]<0)
        continue;
      
      if(ndims==2)
        property = new ElementProperty<real_t>(_mesh->get_coords(n[0]),
                                               _mesh->get_coords(n[1]),
                                               _mesh->get_coords(n[2]));
      else
        property = new ElementProperty<real_t>(_mesh->get_coords(n[0]),
                                               _mesh->get_coords(n[1]),
                                               _mesh->get_coords(n[2]),
                                               _mesh->get_coords(n[3]));
      break;
    }
  }
  
  /// Default destructor.
  ~Coarsen(){
    delete property;
  }

  /*! Perform coarsening.
   * See Figure 15; X Li et al, Comp Methods Appl Mech Engrg 194 (2005) 4915-4950
   */
  void coarsen(real_t L_low, real_t L_max){
    int nprocs = 1, rank = 0;
    if(MPI::Is_initialized()){
      nprocs = MPI::COMM_WORLD.Get_size();
      rank = MPI::COMM_WORLD.Get_rank();
    }
    
    // Initialise a dynamic vertex list
    int NNodes = _mesh->get_number_nodes();
    
    // Initialise list of vertices to be collapsed.
    std::vector<index_t> dynamic_vertex(NNodes, -1);
    std::vector<bool> recalculate_collapse(NNodes, false);
#pragma omp parallel
    {
#pragma omp for schedule(static)
      for(int i=0;i<NNodes;i++){
        if(_mesh->is_owned_node(i))
          dynamic_vertex[i] = coarsen_identify_kernel(i, L_low, L_max);
      }
    }

    // Create the global node numbering.
    int NPNodes = NNodes; // Default for non-mpi
    std::vector<int> lnn2gnn;
    std::vector<size_t> owner;
    
    _mesh->create_global_node_numbering(NPNodes, lnn2gnn, owner);

    // Create a reverse lookup to map received gnn's back to lnn's. 
    std::map<int, int> gnn2lnn;
    for(int i=0;i<NNodes;i++)
      gnn2lnn[lnn2gnn[i]] = i;
    
    // Loop until the maximum independent set is NULL.
    for(int loop=0;loop<100;loop++){
      NNodes = _mesh->get_number_nodes();
      
      if(loop==99)
        std::cerr<<"WARNING: possibly excessive coarsening. Please check results and verify.\n";
      
      // Determine the maximal independent set.
      std::deque<index_t> maximal_independent_set;
      {
        // Colour.
        std::vector<int> colour(NNodes);
        zoltan_colour_graph_t graph;
        graph.rank = rank; 
        
        assert(NNodes==(int)_mesh->NNList.size());
        assert(NNodes==(int)owner.size());
        assert(NNodes==(int)lnn2gnn.size());
        assert(NNodes==(int)gnn2lnn.size());
        assert(NNodes==(int)dynamic_vertex.size());
        assert(NNodes==(int)recalculate_collapse.size());

        graph.nnodes = NNodes;
        
        graph.npnodes = NPNodes;
        
        std::vector<size_t> nedges(NNodes);
        size_t sum = 0;
        for(int i=0;i<NNodes;i++){
          size_t cnt = 0;
          if(owner[i]==(size_t)rank)
            cnt = _mesh->NNList[i].size();
          nedges[i] = cnt;
          sum+=cnt;
        }
        graph.nedges = &(nedges[0]);
        
        std::vector<size_t> csr_edges(sum);
        sum=0;
        for(int i=0;i<NNodes;i++){
          if(owner[i]==(size_t)rank)
            for(typename std::deque<index_t>::iterator it=_mesh->NNList[i].begin();it!=_mesh->NNList[i].end();++it){
              csr_edges[sum++] = *it;
            }
        }
        graph.csr_edges = &(csr_edges[0]);
        
        graph.gid = &(lnn2gnn[0]);
        graph.owner = &(owner[0]);
        
        graph.colour = &(colour[0]);
        
        zoltan_colour(&graph, 2, MPI_COMM_WORLD);

        // Given a colouring, determine the maximum independent set.

        // Create sets of nodes based on colour.
        std::map<int, std::deque<index_t> > colour_sets;
        for(int i=0;i<NNodes;i++){
          if(recalculate_collapse[i]){
            recalculate_collapse[i] = false;
            dynamic_vertex[i] = coarsen_identify_kernel(i, L_low, L_max);
          }
          
          if((colour[i]>=0)&&(dynamic_vertex[i]>=0)){
            colour_sets[colour[i]].push_back(i);
          }
        }
        
        int max_colour = -1;
        if(!colour_sets.empty())
          max_colour = colour_sets.rbegin()->first;
        if(MPI::Is_initialized())
          MPI_Allreduce(MPI_IN_PLACE, &max_colour, 1, MPI_INT, MPI_MAX, _mesh->get_mpi_comm());

        // Check of all vertices have been processed.
        if(max_colour<0){
          break;
        }

        std::vector<int> set_sizes(max_colour, 0);
        for(typename std::map<int, std::deque<index_t> >::const_iterator it=colour_sets.begin();it!=colour_sets.end();++it)
          set_sizes[it->first - 1] = it->second.size();
        
        if(MPI::Is_initialized())
          MPI_Allreduce(MPI_IN_PLACE, &(set_sizes[0]), max_colour, MPI_INT, MPI_SUM, _mesh->get_mpi_comm());

        int max_size=set_sizes[0];
        int max_id=0;
        for(int i=1;i<max_colour;i++)
          if(set_sizes[i]>max_size){
            max_size = set_sizes[i];
            max_id = i;
          }
      
        maximal_independent_set.swap(colour_sets[max_id+1]);
      }

      // Cache who knows what.
      std::vector< std::set<int> > known_nodes(nprocs);
      for(int p=0;p<nprocs;p++){
        if(p==rank)
          continue;
        
        for(std::vector<int>::const_iterator it=_mesh->send[p].begin();it!=_mesh->send[p].end();++it)
          known_nodes[p].insert(*it);
        
        for(std::vector<int>::const_iterator it=_mesh->recv[p].begin();it!=_mesh->recv[p].end();++it)
          known_nodes[p].insert(*it);
      }

      // Communicate collapses.
      if(nprocs>1){
        // Stuff in list of verticies that have to be communicated.
        std::vector< std::vector<int> > send_edges(nprocs);
        std::vector< std::set<int> > send_elements(nprocs), send_nodes(nprocs);
        for(typename std::deque<index_t>::const_iterator it=maximal_independent_set.begin();it!=maximal_independent_set.end();++it){
          // Is the vertex being collapsed contained in the halo?
          if(_mesh->is_halo_node(*it)){ 
            // Yes. Discover where we have to send this edge.
            for(int p=0;p<nprocs;p++){
              if(known_nodes[p].count(*it)){
                send_edges[p].push_back(lnn2gnn[*it]);
                send_edges[p].push_back(lnn2gnn[dynamic_vertex[*it]]);

                send_elements[p].insert(_mesh->NEList[*it].begin(), _mesh->NEList[*it].end());
                //send_elements[p].insert(_mesh->NEList[dynamic_vertex[*it]].begin(), _mesh->NEList[dynamic_vertex[*it]].end());
              }
            }
          }
        }

        // Finalise list of additional elements and nodes to be sent.
        for(int p=0;p<nprocs;p++){
          for(std::set<int>::iterator it=send_elements[p].begin();it!=send_elements[p].end();){
            std::set<int>::iterator ele=it++;
            const int *n=_mesh->get_element(*ele);
            int cnt=0;
            for(size_t i=0;i<nloc;i++){
              if(known_nodes[p].count(n[i])==0){
                send_nodes[p].insert(n[i]);
              }
              if(owner[n[i]]==(size_t)p)
                cnt++;
            }
            if(cnt){
              send_elements[p].erase(ele);
            }
          }
        }

        // Push data to be sent onto the send_buffer.
        std::vector< std::vector<int> > send_buffer(nprocs);
        size_t node_package_int_size = (ndims+1)*ndims*sizeof(real_t)/sizeof(int);
        for(int p=0;p<nprocs;p++){
          if(send_edges[p].size()==0)
            continue;

          // Push on the nodes that need to be communicated.
          send_buffer[p].push_back(send_nodes[p].size());
          for(std::set<int>::iterator it=send_nodes[p].begin();it!=send_nodes[p].end();++it){
            send_buffer[p].push_back(lnn2gnn[*it]);
            send_buffer[p].push_back(owner[*it]);

            // Stuff in coordinates and metric via int's.
            std::vector<int> ivertex(node_package_int_size);
            real_t *rcoords = (real_t *) &(ivertex[0]);
            
            _mesh->get_coords(*it, rcoords);
            _mesh->get_metric(*it, rcoords+ndims);

            send_buffer[p].insert(send_buffer[p].end(), ivertex.begin(), ivertex.end());
          }
          
          // Push on edges that need to be sent.
          send_buffer[p].push_back(send_edges[p].size());
          send_buffer[p].insert(send_buffer[p].end(), send_edges[p].begin(), send_edges[p].end());

          // Push on elements that need to be communicated; record facets that need to be sent with these elements.
          send_buffer[p].push_back(send_elements[p].size());
          std::set<int> send_facets;
          for(std::set<int>::iterator it=send_elements[p].begin();it!=send_elements[p].end();++it){
            const int *n=_mesh->get_element(*it);
            for(size_t j=0;j<nloc;j++)
              send_buffer[p].push_back(lnn2gnn[n[j]]);

            std::vector<int> lfacets;
            _surface->find_facets(n, lfacets);
            send_facets.insert(lfacets.begin(), lfacets.end());
          }

          // Push on facets that need to be communicated.
          send_buffer[p].push_back(send_facets.size());
          for(std::set<int>::iterator it=send_facets.begin();it!=send_facets.end();++it){
            const int *n=_surface->get_facet(*it);
            for(size_t i=0;i<snloc;i++)
              send_buffer[p].push_back(lnn2gnn[n[i]]);
            send_buffer[p].push_back(_surface->get_coplanar_id(*it));
          }
        }
        
        std::vector<int> send_buffer_size(nprocs), recv_buffer_size(nprocs);
        for(int p=0;p<nprocs;p++)
          send_buffer_size[p] = send_buffer[p].size();
        MPI_Alltoall(&(send_buffer_size[0]), 1, MPI_INT, &(recv_buffer_size[0]), 1, MPI_INT, _mesh->get_mpi_comm());
        
        // Setup non-blocking receives
        std::vector< std::vector<int> > recv_buffer(nprocs);
        std::vector<MPI_Request> request(nprocs*2);
        for(int i=0;i<nprocs;i++){
          if(recv_buffer_size[i]==0){
            request[i] =  MPI_REQUEST_NULL;
          }else{
            recv_buffer[i].resize(recv_buffer_size[i]);
            MPI_Irecv(&(recv_buffer[i][0]), recv_buffer_size[i], MPI_INT, i, 0, _mesh->get_mpi_comm(), &(request[i]));
          }
        }
        
        // Non-blocking sends.
        for(int i=0;i<nprocs;i++){
          if(send_buffer_size[i]==0){
            request[nprocs+i] =  MPI_REQUEST_NULL;
          }else{
            MPI_Isend(&(send_buffer[i][0]), send_buffer_size[i], MPI_INT, i, 0, _mesh->get_mpi_comm(), &(request[nprocs+i]));
          }
        }
        
        // Wait for comms to finish.
        std::vector<MPI_Status> status(nprocs*2);
        MPI_Waitall(nprocs, &(request[0]), &(status[0]));
        MPI_Waitall(nprocs, &(request[nprocs]), &(status[nprocs]));
        
        // Unpack received data into dynamic_vertex
        std::vector< std::set<index_t> > extra_halo_receives(nprocs);
        for(int p=0;p<nprocs;p++){
          if(recv_buffer[p].empty())
            continue;

          int loc = 0;

          // Unpack additional nodes.
          int num_extra_nodes = recv_buffer[p][loc++];
          for(int i=0;i<num_extra_nodes;i++){
            int gnn = recv_buffer[p][loc++]; // think this through - can I get duplicates
            int lowner = recv_buffer[p][loc++];

            extra_halo_receives[lowner].insert(gnn);

            real_t *coords = (real_t *) &(recv_buffer[p][loc]);
            real_t *metric = coords + ndims;
            loc+=node_package_int_size;
            
            // Add vertex+metric if we have not already received this data.
            if(gnn2lnn.find(gnn)==gnn2lnn.end()){
              index_t lnn = _mesh->append_vertex(coords, metric);
              
              if(lnn<(index_t)lnn2gnn.size()){
                lnn2gnn[lnn] = gnn;
              }else{
                lnn2gnn.push_back(gnn);
                owner.push_back(lowner);
                dynamic_vertex.push_back(-1);
                recalculate_collapse.push_back(false);
              }
              gnn2lnn[gnn] = lnn;
            }
          }

          // Unpack edges
          size_t edges_size=recv_buffer[p][loc++];
          for(size_t i=0;i<edges_size;i+=2){
            int rm_vertex = gnn2lnn[recv_buffer[p][loc++]];
            int target_vertex = gnn2lnn[recv_buffer[p][loc++]];
            assert(dynamic_vertex[rm_vertex]<0);
            dynamic_vertex[rm_vertex] = target_vertex;
            maximal_independent_set.push_back(rm_vertex);
          }

          // Unpack elements.
          int num_extra_elements = recv_buffer[p][loc++];
          for(int i=0;i<num_extra_elements;i++){
            int element[nloc];
            for(size_t j=0;j<nloc;j++){
              element[j] = gnn2lnn[recv_buffer[p][loc++]];
            }

            // See if this is a new element.
            int cnt=0;
            for(size_t l=0;l<nloc;l++){
              for(size_t k=l+1;k<nloc;k++){
                Edge<real_t, index_t> new_edge(element[l], element[k]);
                typename std::set< Edge<real_t, index_t> >::const_iterator edge = _mesh->Edges.find(new_edge);
                if(edge==_mesh->Edges.end())
                  cnt++;
              }
            }
            
            if(cnt){
              // Add element
              int eid = _mesh->append_element(element);
              
              // Update adjancies: edges, NEList, NNList
              for(size_t l=0;l<nloc;l++){
                _mesh->NEList[element[l]].insert(eid);
                
                for(size_t k=l+1;k<nloc;k++){
                  std::deque<int>::iterator result0 = std::find(_mesh->NNList[element[l]].begin(), _mesh->NNList[element[l]].end(), element[k]);
                  if(result0==_mesh->NNList[element[l]].end())
                    _mesh->NNList[element[l]].push_back(element[k]);
                  
                  std::deque<int>::iterator result1 = std::find(_mesh->NNList[element[k]].begin(), _mesh->NNList[element[k]].end(), element[l]);
                  if(result1==_mesh->NNList[element[k]].end())
                    _mesh->NNList[element[k]].push_back(element[l]);
                  
                  Edge<real_t, index_t> new_edge(element[l], element[k]);
                  typename std::set< Edge<real_t, index_t> >::const_iterator edge = _mesh->Edges.find(new_edge);
                  if(edge!=_mesh->Edges.end()){
                    new_edge.adjacent_elements = edge->adjacent_elements;
                    _mesh->Edges.erase(edge);
                  }
                  
                  new_edge.adjacent_elements.insert(eid);
                  _mesh->Edges.insert(new_edge);
                }
              }
              
            }
          }
          
          // Unpack facets.
          int num_extra_facets = recv_buffer[p][loc++];
          for(int i=0;i<num_extra_facets;i++){
            std::vector<int> facet(snloc);
            for(size_t j=0;j<snloc;j++){
              index_t gnn = recv_buffer[p][loc++];
              assert(gnn2lnn.find(gnn)!=gnn2lnn.end());
              facet[j] = gnn2lnn[gnn];
            }

            int coplanar_id = recv_buffer[p][loc++];

            _surface->append_facet(&(facet[0]), coplanar_id);
            
          }
        }

        assert(gnn2lnn.size()==lnn2gnn.size());

        // Update halo.
        for(int p=0;p<nprocs;p++){
          send_buffer_size[p] = extra_halo_receives[p].size();
          send_buffer[p].clear();
          for(typename std::set<index_t>::const_iterator ht=extra_halo_receives[p].begin();ht!=extra_halo_receives[p].end();++ht)
            send_buffer[p].push_back(*ht);
          
        }
        MPI_Alltoall(&(send_buffer_size[0]), 1, MPI_INT, &(recv_buffer_size[0]), 1, MPI_INT, _mesh->get_mpi_comm());

        // Setup non-blocking receives
        for(int i=0;i<nprocs;i++){
          recv_buffer[i].clear();
          if(recv_buffer_size[i]==0){
            request[i] =  MPI_REQUEST_NULL;
          }else{
            recv_buffer[i].resize(recv_buffer_size[i]);
            MPI_Irecv(&(recv_buffer[i][0]), recv_buffer_size[i], MPI_INT, i, 0, _mesh->get_mpi_comm(), &(request[i]));
          }
        }

        // Non-blocking sends.
        for(int i=0;i<nprocs;i++){
          if(send_buffer_size[i]==0){
            request[nprocs+i] =  MPI_REQUEST_NULL;
          }else{
            MPI_Isend(&(send_buffer[i][0]), send_buffer_size[i], MPI_INT, i, 0, _mesh->get_mpi_comm(), &(request[nprocs+i]));
          }
        }

        // Wait for comms to finish.
        MPI_Waitall(nprocs, &(request[0]), &(status[0]));
        MPI_Waitall(nprocs, &(request[nprocs]), &(status[nprocs]));

        // Use this data to update the halo information.
        for(int i=0;i<nprocs;i++){
          for(std::vector<int>::const_iterator it=recv_buffer[i].begin();it!=recv_buffer[i].end();++it){
            assert(gnn2lnn.find(*it)!=gnn2lnn.end());
            int lnn = gnn2lnn[*it];
            _mesh->send[i].push_back(lnn);
            _mesh->send_halo.insert(lnn);
          }
          for(std::vector<int>::const_iterator it=send_buffer[i].begin();it!=send_buffer[i].end();++it){
            assert(gnn2lnn.find(*it)!=gnn2lnn.end());
            int lnn = gnn2lnn[*it];
            _mesh->recv[i].push_back(lnn);
            _mesh->recv_halo.insert(lnn);
          }
        }
      }

      assert(gnn2lnn.size()==lnn2gnn.size());

      // Perform collapse operations.
      {
        int node_set_size = maximal_independent_set.size();
        for(int i=0;i<node_set_size;i++){
          // Vertex to be removed: rm_vertex
          int rm_vertex=maximal_independent_set[i];
          int target_vertex=dynamic_vertex[rm_vertex];
          assert(target_vertex>=0);

          if(target_vertex<0)
            continue;
          
          // Call the coarsening kernel.
          coarsen_kernel(rm_vertex, target_vertex);
          
          if(_mesh->is_owned_node(target_vertex)){
            dynamic_vertex[target_vertex] = coarsen_identify_kernel(target_vertex, L_low, L_max);
            assert(dynamic_vertex[target_vertex]!=rm_vertex);
          }

          for(typename std::deque<index_t>::iterator it=_mesh->NNList[target_vertex].begin();it!=_mesh->NNList[target_vertex].end();++it)
            recalculate_collapse[*it] = true;
          
          dynamic_vertex[rm_vertex] = -1;
        }
      }

      assert(gnn2lnn.size()==lnn2gnn.size());
    }
    
    return;
  }
  
  /*! Kernel for identifying what if any vertex rm_vertex should be collapsed onto.
   * See Figure 15; X Li et al, Comp Methods Appl Mech Engrg 194 (2005) 4915-4950
   * Returns the node ID that rm_vertex should be collapsed onto, negative if no operation is to be performed.
   */
  int coarsen_identify_kernel(index_t rm_vertex, real_t L_low, real_t L_max){    
    // If this is a corner-vertex then cannot collapse;
    if(_surface->is_corner_vertex(rm_vertex))
      return -2;
    
    // If this is not owned then return -1.
    if(!_mesh->is_owned_node(rm_vertex))
      return -3;

    /* Sort the edges according to length. We want to collapse the
       shortest. If it's not possible to collapse the edge then move
       onto the next shortest.*/
    std::multimap<real_t, const Edge<real_t, index_t>* > short_edges;
    for(typename std::deque<index_t>::const_iterator nn=_mesh->NNList[rm_vertex].begin();nn!=_mesh->NNList[rm_vertex].end();++nn){
      // For now impose the restriction that we will not coarsen across partition boundarys.
      if(_mesh->recv_halo.count(*nn))
        continue;

      // First check if this edge can be collapsed
      if(!_surface->is_collapsible(rm_vertex, *nn))
        continue;
      
      typename std::set< Edge<real_t, index_t> >::const_iterator edge = _mesh->Edges.find(Edge<real_t, index_t>(rm_vertex, *nn));
      assert(edge!=_mesh->Edges.end());
      if(edge->length<L_low)
        short_edges.insert(std::pair< real_t, const Edge<real_t, index_t>*  >(edge->length, &(*edge)));
    }
    
    bool reject_collapse = false;
    const Edge<real_t, index_t> *target_edge = NULL;
    index_t target_vertex=-1;
    while(short_edges.size()){
      // Get the next shortest edge.
      target_edge = short_edges.begin()->second;
      short_edges.erase(short_edges.begin());

      // Assume the best.
      reject_collapse=false;

      // Identify vertex that will be collapsed onto.
      target_vertex = (rm_vertex==target_edge->edge.first)?target_edge->edge.second:target_edge->edge.first;
      
      // Check the properties of new elements. If the new properties
      // are not acceptable when continue.
      for(typename std::set<index_t>::iterator ee=_mesh->NEList[rm_vertex].begin();ee!=_mesh->NEList[rm_vertex].end();++ee){
        if(target_edge->adjacent_elements.count(*ee))
          continue;
        
        // Create a copy of the proposed element
        std::vector<int> n(nloc);
        const int *orig_n=_mesh->get_element(*ee);
        for(size_t i=0;i<nloc;i++){
          int nid = orig_n[i];
          if(nid==rm_vertex)
            n[i] = target_vertex;
          else
            n[i] = nid;
        }
        
        // Check the volume of this new element.
        double orig_volume, volume;
        if(ndims==2){
          orig_volume = property->area(_mesh->get_coords(orig_n[0]),
                                       _mesh->get_coords(orig_n[1]),
                                       _mesh->get_coords(orig_n[2]));

          volume = property->area(_mesh->get_coords(n[0]),
                                  _mesh->get_coords(n[1]),
                                  _mesh->get_coords(n[2]));
        }else{
          orig_volume = property->volume(_mesh->get_coords(orig_n[0]),
                                         _mesh->get_coords(orig_n[1]),
                                         _mesh->get_coords(orig_n[2]),
                                         _mesh->get_coords(orig_n[3]));

          volume = property->volume(_mesh->get_coords(n[0]),
                                    _mesh->get_coords(n[1]),
                                    _mesh->get_coords(n[2]),
                                    _mesh->get_coords(n[3]));
        }

        if(volume/orig_volume<=1.0e-3){
          reject_collapse=true;
          break;
        }
      }

      // Check of any of the new edges are longer than L_max.
      for(typename std::deque<index_t>::const_iterator nn=_mesh->NNList[rm_vertex].begin();nn!=_mesh->NNList[rm_vertex].end();++nn){
        if(target_vertex==*nn)
          continue;
        
        if(_mesh->calc_edge_length(target_vertex, *nn)>L_max){
          reject_collapse=true;
          break;
        }
      }
      
      // If this edge is ok to collapse then jump out.
      if(!reject_collapse)
        break;
    }
    
    // If we're checked all edges and none are collapsible then return.
    if(reject_collapse)
      return -4;
    
    return target_vertex;
  }

  /*! Kernel for perform coarsening.
   * See Figure 15; X Li et al, Comp Methods Appl Mech Engrg 194 (2005) 4915-4950
   * Returns the node ID that rm_vertex is collapsed onto, negative if the operation is not performed.
   */
  int coarsen_kernel(index_t rm_vertex, index_t target_vertex){
    typename std::set< Edge<real_t, index_t> >::const_iterator edge_iterator = _mesh->Edges.find(Edge<real_t, index_t>(rm_vertex, target_vertex));
    assert(edge_iterator!=_mesh->Edges.end());

    const Edge<real_t, index_t> *target_edge = &(*edge_iterator);
    
    std::set<index_t> deleted_elements = target_edge->adjacent_elements;
    
    // Perform coarsening on surface if necessary.
    if(_surface->contains_node(rm_vertex)&&_surface->contains_node(target_vertex))
      _surface->collapse(rm_vertex, target_vertex);

    // Remove deleted elements from node-elemement adjancy list.
    for(typename std::set<index_t>::const_iterator de=deleted_elements.begin(); de!=deleted_elements.end();++de){
      const int *n=_mesh->get_element(*de);
      assert(n[0]>=0);
      for(size_t i=0;i<nloc;i++){
        for(size_t j=i+1;j<nloc;j++){
          typename std::set< Edge<real_t, index_t> >::iterator iother_edge = _mesh->Edges.find(Edge<real_t, index_t>(n[i], n[j]));
          if(*iother_edge==*target_edge)
            continue;
          assert(iother_edge!=_mesh->Edges.end());
          Edge<real_t, index_t> new_edge = *iother_edge;
          _mesh->Edges.erase(iother_edge);
          
          new_edge.adjacent_elements.erase(*de);
          _mesh->Edges.insert(new_edge);
        }
      }
    }
    
    // Renumber nodes in elements adjacent to rm_vertex, deleted
    // elements being collapsed, and make these elements adjacent to
    // target_vertex.
    for(typename std::set<index_t>::iterator ee=_mesh->NEList[rm_vertex].begin();ee!=_mesh->NEList[rm_vertex].end();++ee){
      // Delete if element is to be collapsed.
      if(deleted_elements.count(*ee)){
        _mesh->erase_element(*ee);
      }else{
        // Renumber
        for(size_t i=0;i<nloc;i++){
          if(_mesh->_ENList[nloc*(*ee)+i]==rm_vertex){
            _mesh->_ENList[nloc*(*ee)+i]=target_vertex;
            break;
          }
        }
        
        // Add element to target node-elemement adjancy list.
        _mesh->NEList[target_vertex].insert(*ee);
      }
    }

    // Remove deleted elements from node-elemement adjancy list.
    for(typename std::set<index_t>::const_iterator de=deleted_elements.begin(); de!=deleted_elements.end();++de){
      _mesh->NEList[target_vertex].erase(*de);
    }

    // Update Edges.
    std::set<index_t> adj_nodes_target = _mesh->get_node_patch(target_vertex);
    for(typename std::deque<index_t>::const_iterator nn=_mesh->NNList[rm_vertex].begin();nn!=_mesh->NNList[rm_vertex].end();++nn){      
      // We have to extract a copy of the edge being edited.
      typename std::set< Edge<real_t, index_t> >::iterator iedge_modify = _mesh->Edges.find(Edge<real_t, index_t>(rm_vertex, *nn));

      assert(iedge_modify!=_mesh->Edges.end());
      Edge<real_t, index_t> edge_modify = *iedge_modify;
      _mesh->Edges.erase(iedge_modify);
      
      // Continue is this is the target edge.
      if(target_vertex==*nn)
        continue;
  
      // Update vertex id's for this edge.
      edge_modify.edge.first = std::min(target_vertex, *nn);
      edge_modify.edge.second = std::max(target_vertex, *nn);
      
      // Check if this edge is being collapsed onto an existing edge connected to target vertex.
      if(adj_nodes_target.count(*nn)){
        typename std::set< Edge<real_t, index_t> >::iterator iedge_duplicate = _mesh->Edges.find(Edge<real_t, index_t>(target_vertex, *nn));
        assert(iedge_duplicate!=_mesh->Edges.end());
        Edge<real_t, index_t> edge_duplicate = *iedge_duplicate;
        _mesh->Edges.erase(iedge_duplicate);

        // Add in additional elements from edge being merged onto.
        edge_modify.adjacent_elements.insert(edge_duplicate.adjacent_elements.begin(),
                                             edge_duplicate.adjacent_elements.end());
        
        // Copy the length
        edge_modify.length = edge_duplicate.length;
      }else{
        // Update the length of the edge in metric space.
        edge_modify.length = _mesh->calc_edge_length(target_vertex, *nn);
      }
      
      // Add in modified edge back in.
      _mesh->Edges.insert(edge_modify);
    }
    
    // Update surrounding NNList and add elements to ENList.
    for(typename std::deque<index_t>::const_iterator nn=_mesh->NNList[rm_vertex].begin();nn!=_mesh->NNList[rm_vertex].end();++nn){
      if(*nn == target_vertex){
        std::set<index_t> new_patch = adj_nodes_target;
        for(typename std::deque<index_t>::const_iterator inn=_mesh->NNList[rm_vertex].begin();inn!=_mesh->NNList[rm_vertex].end();++inn)
          new_patch.insert(*inn);
        new_patch.erase(target_vertex);
        new_patch.erase(rm_vertex);
        _mesh->NNList[*nn].clear();
        for(typename std::set<index_t>::const_iterator inn=new_patch.begin();inn!=new_patch.end();++inn)
          _mesh->NNList[*nn].push_back(*inn);
      }else if(adj_nodes_target.count(*nn)){
        // Delete element adjancies from NEList.
        for(typename std::set<index_t>::const_iterator de=deleted_elements.begin();de!=deleted_elements.end();++de){
          typename std::set<index_t>::iterator ele = _mesh->NEList[*nn].find(*de);
          if(ele!=_mesh->NEList[*nn].end())
            _mesh->NEList[*nn].erase(ele);
        }
        
        // Deletes edges from NNList
        typename std::deque<index_t>::iterator back_reference = find(_mesh->NNList[*nn].begin(),
                                                                     _mesh->NNList[*nn].end(), rm_vertex);
        assert(back_reference!=_mesh->NNList[*nn].end());
        _mesh->NNList[*nn].erase(back_reference);
      }else{
        typename std::deque<index_t>::iterator back_reference = find(_mesh->NNList[*nn].begin(),
                                                                     _mesh->NNList[*nn].end(), rm_vertex);
        assert(back_reference!=_mesh->NNList[*nn].end());
        *back_reference = target_vertex;
      }
    }
    
    _mesh->erase_vertex(rm_vertex);
  
    return target_vertex;
  }

 private:
  Mesh<real_t, index_t> *_mesh;
  Surface<real_t, index_t> *_surface;
  ElementProperty<real_t> *property;
  size_t ndims, nloc, snloc;
};

#endif
