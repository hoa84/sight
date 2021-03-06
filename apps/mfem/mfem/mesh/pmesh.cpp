// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.googlecode.com.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#ifdef MFEM_USE_MPI

#include "mesh_headers.hpp"
#include "../fem/fem.hpp"
#include "../general/sets.hpp"

#include "dbglog.hpp"

ParMesh::ParMesh(MPI_Comm comm, Mesh &mesh, int *partitioning_,
                 int part_method)
   : gtopo(comm)
{
   int i, j;
   int *partitioning;
   Array<bool> activeBdrElem;

   MyComm = comm;
   MPI_Comm_size(MyComm, &NRanks);
   MPI_Comm_rank(MyComm, &MyRank);

   Dim = mesh.Dim;

   if (partitioning_)
      partitioning = partitioning_;
   else
      partitioning = mesh.GeneratePartitioning(NRanks, part_method);

   // re-enumerate the partitions to better map to actual processor
   // interconnect topology !?

   Array<int> vert;
   Array<int> vert_global_local(mesh.GetNV());
   int vert_counter, element_counter, bdrelem_counter;

   // build vert_global_local
   vert_global_local = -1;

   element_counter = 0;
   vert_counter = 0;
   for (i = 0; i < mesh.GetNE(); i++)
      if (partitioning[i] == MyRank)
      {
         mesh.GetElementVertices(i, vert);
         element_counter++;
         for (j = 0; j < vert.Size(); j++)
            if (vert_global_local[vert[j]] < 0)
               vert_global_local[vert[j]] = vert_counter++;
      }

   NumOfVertices = vert_counter;
   NumOfElements = element_counter;
   vertices.SetSize(NumOfVertices);

   // re-enumerate the local vertices to preserve the global ordering
   for (i = vert_counter = 0; i < vert_global_local.Size(); i++)
      if (vert_global_local[i] >= 0)
         vert_global_local[i] = vert_counter++;

   // determine vertices
   for (i = 0; i < vert_global_local.Size(); i++)
      if (vert_global_local[i] >= 0)
         vertices[vert_global_local[i]].SetCoords(mesh.GetVertex(i));

   // determine elements
   element_counter = 0;
   elements.SetSize(NumOfElements);
   for (i = 0; i < mesh.GetNE(); i++)
      if (partitioning[i] == MyRank)
      {
         elements[element_counter] = mesh.GetElement(i)->Duplicate(this);
         int *v = elements[element_counter]->GetVertices();
         int nv = elements[element_counter]->GetNVertices();
         for (j = 0; j < nv; j++)
            v[j] = vert_global_local[v[j]];
         element_counter++;
      }

   Table *edge_element = NULL;
   if (mesh.NURBSext)
   {
      activeBdrElem.SetSize(mesh.GetNBE());
      activeBdrElem = false;
   }
   // build boundary elements
   if (Dim == 3)
   {
      NumOfBdrElements = 0;
      for (i = 0; i < mesh.GetNBE(); i++)
      {
         int face = mesh.GetBdrElementEdgeIndex(i);
         int el1, el2;
         mesh.GetFaceElements(face, &el1, &el2);
         if (partitioning[el1] == MyRank)
         {
            NumOfBdrElements++;
            if (mesh.NURBSext)
               activeBdrElem[i] = true;
         }
      }

      bdrelem_counter = 0;
      boundary.SetSize(NumOfBdrElements);
      for (i = 0; i < mesh.GetNBE(); i++)
      {
         int face = mesh.GetBdrElementEdgeIndex(i);
         int el1, el2;
         mesh.GetFaceElements(face, &el1, &el2);
         if (partitioning[el1] == MyRank)
         {
            boundary[bdrelem_counter] = mesh.GetBdrElement(i)->Duplicate(this);
            int *v = boundary[bdrelem_counter]->GetVertices();
            int nv = boundary[bdrelem_counter]->GetNVertices();
            for (j = 0; j < nv; j++)
               v[j] = vert_global_local[v[j]];
            bdrelem_counter++;
         }
      }

   }
   else if (Dim == 2)
   {
      edge_element = new Table;
      Transpose(mesh.ElementToEdgeTable(), *edge_element, mesh.GetNEdges());

      NumOfBdrElements = 0;
      for (i = 0; i < mesh.GetNBE(); i++)
      {
         int edge = mesh.GetBdrElementEdgeIndex(i);
         int el1 = edge_element->GetRow(edge)[0];
         if (partitioning[el1] == MyRank)
         {
            NumOfBdrElements++;
            if (mesh.NURBSext)
               activeBdrElem[i] = true;
         }
      }

      bdrelem_counter = 0;
      boundary.SetSize(NumOfBdrElements);
      for (i = 0; i < mesh.GetNBE(); i++)
      {
         int edge = mesh.GetBdrElementEdgeIndex(i);
         int el1 = edge_element->GetRow(edge)[0];
         if (partitioning[el1] == MyRank)
         {
            boundary[bdrelem_counter] = mesh.GetBdrElement(i)->Duplicate(this);
            int *v = boundary[bdrelem_counter]->GetVertices();
            int nv = boundary[bdrelem_counter]->GetNVertices();
            for (j = 0; j < nv; j++)
               v[j] = vert_global_local[v[j]];
            bdrelem_counter++;
         }
      }
   }

   meshgen = mesh.MeshGenerator();

   mesh.attributes.Copy(attributes);
   mesh.bdr_attributes.Copy(bdr_attributes);

   // this is called by the default Mesh constructor
   // InitTables();

   el_to_edge = new Table;
   NumOfEdges = Mesh::GetElementToEdgeTable(*el_to_edge, be_to_edge);

   STable3D *faces_tbl = NULL;
   if (Dim == 3)
      faces_tbl = GetElementToFaceTable(1);
   else
      NumOfFaces = 0;
   GenerateFaces();

   c_el_to_edge = NULL;

   ListOfIntegerSets  groups;
   IntegerSet         group;

   // the first group is the local one
   group.Recreate(1, &MyRank);
   groups.Insert(group);

#ifdef MFEM_DEBUG
   if (Dim < 3 && mesh.GetNFaces() != 0)
   {
      cerr << "ParMesh::ParMesh (proc " << MyRank << ") : "
         "(Dim < 3 && mesh.GetNFaces() != 0) is true!" << endl;
      mfem_error();
   }
#endif
   // determine shared faces
   int sface_counter = 0;
   Array<int> face_group(mesh.GetNFaces());
   for (i = 0; i < face_group.Size(); i++)
   {
      int el[2];
      face_group[i] = -1;
      mesh.GetFaceElements(i, &el[0], &el[1]);
      if (el[1] >= 0)
      {
         el[0] = partitioning[el[0]];
         el[1] = partitioning[el[1]];
         if ((el[0] == MyRank && el[1] != MyRank) ||
             (el[0] != MyRank && el[1] == MyRank))
         {
            group.Recreate(2, el);
            face_group[i] = groups.Insert(group) - 1;
            sface_counter++;
         }
      }
   }

   // determine shared edges
   int sedge_counter = 0;
   if (!edge_element)
   {
      edge_element = new Table;
      Transpose(mesh.ElementToEdgeTable(), *edge_element, mesh.GetNEdges());
   }
   for (i = 0; i < edge_element->Size(); i++)
   {
      int me = 0, others = 0;
      for (j = edge_element->GetI()[i]; j < edge_element->GetI()[i+1]; j++)
      {
         edge_element->GetJ()[j] = partitioning[edge_element->GetJ()[j]];
         if (edge_element->GetJ()[j] == MyRank)
            me = 1;
         else
            others = 1;
      }

      if (me && others)
      {
         sedge_counter++;
         group.Recreate(edge_element->RowSize(i), edge_element->GetRow(i));
         edge_element->GetRow(i)[0] = groups.Insert(group) - 1;
      }
      else
         edge_element->GetRow(i)[0] = -1;
   }

   // determine shared vertices
   int svert_counter = 0;
   Table *vert_element = mesh.GetVertexToElementTable(); // we must delete this

   for (i = 0; i < vert_element->Size(); i++)
   {
      int me = 0, others = 0;
      for (j = vert_element->GetI()[i]; j < vert_element->GetI()[i+1]; j++)
      {
         vert_element->GetJ()[j] = partitioning[vert_element->GetJ()[j]];
         if (vert_element->GetJ()[j] == MyRank)
            me = 1;
         else
            others = 1;
      }

      if (me && others)
      {
         svert_counter++;
         group.Recreate(vert_element->RowSize(i), vert_element->GetRow(i));
         vert_element->GetRow(i)[0] = groups.Insert(group) - 1;
      }
      else
         vert_element->GetRow(i)[0] = -1;
   }

   // build group_sface
   group_sface.MakeI(groups.Size()-1);

   for (i = 0; i < face_group.Size(); i++)
      if (face_group[i] >= 0)
         group_sface.AddAColumnInRow(face_group[i]);

   group_sface.MakeJ();

   sface_counter = 0;
   for (i = 0; i < face_group.Size(); i++)
      if (face_group[i] >= 0)
         group_sface.AddConnection(face_group[i], sface_counter++);

   group_sface.ShiftUpI();

   // build group_sedge
   group_sedge.MakeI(groups.Size()-1);

   for (i = 0; i < edge_element->Size(); i++)
      if (edge_element->GetRow(i)[0] >= 0)
         group_sedge.AddAColumnInRow(edge_element->GetRow(i)[0]);

   group_sedge.MakeJ();

   sedge_counter = 0;
   for (i = 0; i < edge_element->Size(); i++)
      if (edge_element->GetRow(i)[0] >= 0)
         group_sedge.AddConnection(edge_element->GetRow(i)[0],
                                   sedge_counter++);

   group_sedge.ShiftUpI();

   // build group_svert
   group_svert.MakeI(groups.Size()-1);

   for (i = 0; i < vert_element->Size(); i++)
      if (vert_element->GetRow(i)[0] >= 0)
         group_svert.AddAColumnInRow(vert_element->GetRow(i)[0]);

   group_svert.MakeJ();

   svert_counter = 0;
   for (i = 0; i < vert_element->Size(); i++)
      if (vert_element->GetRow(i)[0] >= 0)
         group_svert.AddConnection(vert_element->GetRow(i)[0],
                                   svert_counter++);

   group_svert.ShiftUpI();

   // build shared_faces and sface_lface
   shared_faces.SetSize(sface_counter);
   sface_lface. SetSize(sface_counter);

   if (Dim == 3)
   {
      sface_counter = 0;
      for (i = 0; i < face_group.Size(); i++)
         if (face_group[i] >= 0)
         {
            shared_faces[sface_counter] = mesh.GetFace(i)->Duplicate(this);
            int *v = shared_faces[sface_counter]->GetVertices();
            int nv = shared_faces[sface_counter]->GetNVertices();
            for (j = 0; j < nv; j++)
               v[j] = vert_global_local[v[j]];
            switch (shared_faces[sface_counter]->GetType())
            {
            case Element::TRIANGLE:
               sface_lface[sface_counter] = (*faces_tbl)(v[0], v[1], v[2]);
               // mark the shared face for refinement by reorienting
               // it according to the refinement flag in the tetradron
               // to which this shared face belongs to.
               {
                  int lface = sface_lface[sface_counter];
                  Tetrahedron *tet =
                     (Tetrahedron *)(elements[faces_info[lface].Elem1No]);
                  int re[2], type, flag, *tv;
                  tet->ParseRefinementFlag(re, type, flag);
                  tv = tet->GetVertices();
                  switch (faces_info[lface].Elem1Inf/64)
                  {
                  case 0:
                     switch (re[1])
                     {
                     case 1: v[0] = tv[1]; v[1] = tv[2]; v[2] = tv[3]; break;
                     case 4: v[0] = tv[3]; v[1] = tv[1]; v[2] = tv[2]; break;
                     case 5: v[0] = tv[2]; v[1] = tv[3]; v[2] = tv[1]; break;
                     }
                     break;
                  case 1:
                     switch (re[0])
                     {
                     case 2: v[0] = tv[2]; v[1] = tv[0]; v[2] = tv[3]; break;
                     case 3: v[0] = tv[0]; v[1] = tv[3]; v[2] = tv[2]; break;
                     case 5: v[0] = tv[3]; v[1] = tv[2]; v[2] = tv[0]; break;
                     }
                     break;
                  case 2:
                     v[0] = tv[0]; v[1] = tv[1]; v[2] = tv[3];
                     break;
                  case 3:
                     v[0] = tv[1]; v[1] = tv[0]; v[2] = tv[2];
                     break;
                  }
                  // flip the shared face in the processor that owns the
                  // second element (in 'mesh')
                  {
                     int gl_el1, gl_el2;
                     mesh.GetFaceElements(i, &gl_el1, &gl_el2);
                     if (MyRank == partitioning[gl_el2])
                     {
                        const int t = v[0]; v[0] = v[1]; v[1] = t;
                     }
                  }
               }
               break;
            case Element::QUADRILATERAL:
               sface_lface[sface_counter] =
                  (*faces_tbl)(v[0], v[1], v[2], v[3]);
               break;
            }
            sface_counter++;
         }

      delete faces_tbl;
   }

   // build shared_edges and sedge_ledge
   shared_edges.SetSize(sedge_counter);
   sedge_ledge. SetSize(sedge_counter);

   {
      DSTable v_to_v(NumOfVertices);
      GetVertexToVertexTable(v_to_v);

      sedge_counter = 0;
      for (i = 0; i < edge_element->Size(); i++)
         if (edge_element->GetRow(i)[0] >= 0)
         {
            mesh.GetEdgeVertices(i, vert);

            shared_edges[sedge_counter] =
               new Segment(vert_global_local[vert[0]],
                           vert_global_local[vert[1]], 1);

            if ((sedge_ledge[sedge_counter] =
                 v_to_v(vert_global_local[vert[0]],
                        vert_global_local[vert[1]])) < 0)
            {
               cerr << "\n\n\n" << MyRank << ": ParMesh::ParMesh: "
                    << "ERROR in v_to_v\n\n" << endl;
               mfem_error();
            }

            sedge_counter++;
         }
   }

   delete edge_element;

   // build svert_lvert
   svert_lvert.SetSize(svert_counter);

   svert_counter = 0;
   for (i = 0; i < vert_element->Size(); i++)
      if (vert_element->GetRow(i)[0] >= 0)
         svert_lvert[svert_counter++] = vert_global_local[i];

   delete vert_element;

   // build the group communication topology
   gtopo.Create(groups, 822);

   if (mesh.NURBSext)
   {
      NURBSext = new ParNURBSExtension(comm, mesh.NURBSext, partitioning,
                                       activeBdrElem);
   }

   if (mesh.GetNodes()) // curved mesh
   {
      Nodes = new ParGridFunction(this, mesh.GetNodes());
      own_nodes = 1;

      Array<int> gvdofs, lvdofs;
      Vector lnodes;
      element_counter = 0;
      for (i = 0; i < mesh.GetNE(); i++)
         if (partitioning[i] == MyRank)
         {
            Nodes->FESpace()->GetElementVDofs(element_counter, lvdofs);
            mesh.GetNodes()->FESpace()->GetElementVDofs(i, gvdofs);
            mesh.GetNodes()->GetSubVector(gvdofs, lnodes);
            Nodes->SetSubVector(lvdofs, lnodes);
            element_counter++;
         }
   }

   if (NURBSext == NULL)
      delete [] partitioning;
}

void ParMesh::GroupEdge(int group, int i, int &edge, int &o)
{
   int sedge = group_sedge.GetJ()[group_sedge.GetI()[group-1]+i];
   edge = sedge_ledge[sedge];
   int *v = shared_edges[sedge]->GetVertices();
   o = (v[0] < v[1]) ? (+1) : (-1);
}

void ParMesh::GroupFace(int group, int i, int &face, int &o)
{
   int sface = group_sface.GetJ()[group_sface.GetI()[group-1]+i];
   face = sface_lface[sface];
   // face gives the base orientation
   if (faces[face]->GetType() == Element::TRIANGLE)
      o = GetTriOrientation(faces[face]->GetVertices(),
                            shared_faces[sface]->GetVertices());
   if (faces[face]->GetType() == Element::QUADRILATERAL)
      o = GetQuadOrientation(faces[face]->GetVertices(),
                             shared_faces[sface]->GetVertices());
}

// For a line segment with vertices v[0] and v[1], return a number with
// the following meaning:
// 0 - the edge was not refined
// 1 - the edge e was refined once by splitting v[0],v[1]
int ParMesh::GetEdgeSplittings(Element *edge, const DSTable &v_to_v,
                               int *middle)
{
   int m, *v = edge->GetVertices();

   if ((m = v_to_v(v[0], v[1])) != -1 && middle[m] != -1)
      return 1;
   else
      return 0;
}

// For a triangular face with (correctly ordered) vertices v[0], v[1], v[2]
// return a number with the following meaning:
// 0 - the face was not refined
// 1 - the face was refined once by splitting v[0],v[1]
// 2 - the face was refined twice by splitting v[0],v[1] and then v[1],v[2]
// 3 - the face was refined twice by splitting v[0],v[1] and then v[0],v[2]
// 4 - the face was refined three times (as in 2+3)
int ParMesh::GetFaceSplittings(Element *face, const DSTable &v_to_v,
                               int *middle)
{
   int m, right = 0;
   int number_of_splittings = 0;
   int *v = face->GetVertices();

   if ((m = v_to_v(v[0], v[1])) != -1 && middle[m] != -1)
   {
      number_of_splittings++;
      if ((m = v_to_v(v[1], v[2])) != -1 && middle[m] != -1)
      {
         right = 1;
         number_of_splittings++;
      }
      if ((m = v_to_v(v[2], v[0])) != -1 && middle[m] != -1)
         number_of_splittings++;

      switch (number_of_splittings)
      {
      case 2:
         if (right == 0)
            number_of_splittings++;
         break;
      case 3:
         number_of_splittings++;
         break;
      }
   }

   return number_of_splittings;
}

void ParMesh::ReorientTetMesh()
{
   if (Dim != 3 || !(meshgen & 1))
      return;

   Mesh::ReorientTetMesh();

   int *v;

   // The local edge and face numbering is changed therefore we need to
   // update sedge_ledge and sface_lface.
   {
      DSTable v_to_v(NumOfVertices);
      GetVertexToVertexTable(v_to_v);
      for (int i = 0; i < shared_edges.Size(); i++)
      {
         v = shared_edges[i]->GetVertices();
         sedge_ledge[i] = v_to_v(v[0], v[1]);
      }
   }

   // Rotate shared faces and update sface_lface.
   // Note that no communication is needed to ensure that the shared
   // faces are rotated in the same way in both processors. This is
   // automatic due to various things, e.g. the global to local vertex
   // mapping preserves the global order; also the way new vertices
   // are introduced during refinement is essential.
   {
      STable3D *faces_tbl = GetFacesTable();
      for (int i = 0; i < shared_faces.Size(); i++)
         if (shared_faces[i]->GetType() == Element::TRIANGLE)
         {
            v = shared_faces[i]->GetVertices();

            Rotate3(v[0], v[1], v[2]);

            sface_lface[i] = (*faces_tbl)(v[0], v[1], v[2]);
         }
      delete faces_tbl;
   }
}

void ParMesh::LocalRefinement(const Array<int> &marked_el, int type)
{
   int i, j, wtls = WantTwoLevelState;

   if (Nodes)  // curved mesh
   {
      UseTwoLevelState(1);
   }

   SetState(Mesh::NORMAL);
   DeleteCoarseTables();

   if (Dim == 3)
   {
      if (WantTwoLevelState)
      {
         c_NumOfVertices    = NumOfVertices;
         c_NumOfEdges       = NumOfEdges;
         c_NumOfFaces       = NumOfFaces;
         c_NumOfElements    = NumOfElements;
         c_NumOfBdrElements = NumOfBdrElements;
      }

      int uniform_refinement = 0;
      if (type < 0)
      {
         type = -type;
         uniform_refinement = 1;
      }

      // 1. Get table of vertex to vertex connections.
      DSTable v_to_v(NumOfVertices);
      GetVertexToVertexTable(v_to_v);

      // 2. Get edge to element connections in arrays edge1 and edge2
      Array<int> middle(v_to_v.NumberOfEntries());
      middle = -1;

      // 3. Do the red refinement.
      switch (type)
      {
      case 1:
         for (i = 0; i < marked_el.Size(); i++)
            Bisection(marked_el[i], v_to_v, NULL, NULL, middle);
         break;
      case 2:
         for (i = 0; i < marked_el.Size(); i++)
         {
            Bisection(marked_el[i], v_to_v, NULL, NULL, middle);

            Bisection(NumOfElements - 1, v_to_v, NULL, NULL, middle);
            Bisection(marked_el[i], v_to_v, NULL, NULL, middle);
         }
         break;
      case 3:
         for (i = 0; i < marked_el.Size(); i++)
         {
            Bisection(marked_el[i], v_to_v, NULL, NULL, middle);

            j = NumOfElements - 1;
            Bisection(j, v_to_v, NULL, NULL, middle);
            Bisection(NumOfElements - 1, v_to_v, NULL, NULL, middle);
            Bisection(j, v_to_v, NULL, NULL, middle);

            Bisection(marked_el[i], v_to_v, NULL, NULL, middle);
            Bisection(NumOfElements-1, v_to_v, NULL, NULL, middle);
            Bisection(marked_el[i], v_to_v, NULL, NULL, middle);
         }
         break;
      }

      if (WantTwoLevelState)
      {
         RefinedElement::State = RefinedElement::FINE;
         State = Mesh::TWO_LEVEL_FINE;
      }

      // 4. Do the green refinement (to get conforming mesh).
      int need_refinement;
      int refined_edge[5][3] = {{0, 0, 0},
                                {1, 0, 0},
                                {1, 1, 0},
                                {1, 0, 1},
                                {1, 1, 1}};
      int faces_in_group, max_faces_in_group = 0;
      // face_splittings identify how the shared faces have been split
      int **face_splittings = new int*[GetNGroups()-1];
      for (i = 0; i < GetNGroups()-1; i++)
      {
         faces_in_group = GroupNFaces(i+1);
         face_splittings[i] = new int[faces_in_group];
         if (faces_in_group > max_faces_in_group)
            max_faces_in_group = faces_in_group;
      }
      int neighbor, *iBuf = new int[max_faces_in_group];

      Array<int> group_faces;
      Vertex V;

      MPI_Request request;
      MPI_Status  status;

#ifdef MFEM_DEBUG
      int ref_loops_all = 0, ref_loops_par = 0;
#endif
      do
      {
         need_refinement = 0;
         for (i = 0; i < NumOfElements; i++)
         {
            if (elements[i]->NeedRefinement(v_to_v, middle))
            {
               need_refinement = 1;
               Bisection(i, v_to_v, NULL, NULL, middle);
            }
         }
#ifdef MFEM_DEBUG
         ref_loops_all++;
#endif

         if (uniform_refinement)
            continue;

         // if the mesh is locally conforming start making it globally
         // conforming
         if (need_refinement == 0)
         {
#ifdef MFEM_DEBUG
            ref_loops_par++;
#endif
            // MPI_Barrier(MyComm);

            // (a) send the type of interface splitting
            for (i = 0; i < GetNGroups()-1; i++)
            {
               group_sface.GetRow(i, group_faces);
               faces_in_group = group_faces.Size();
               // it is enough to communicate through the faces
               if (faces_in_group != 0)
               {
                  for (j = 0; j < faces_in_group; j++)
                     face_splittings[i][j] =
                        GetFaceSplittings(shared_faces[group_faces[j]], v_to_v,
                                          middle);
                  const int *nbs = gtopo.GetGroup(i+1);
                  if (nbs[0] == 0)
                     neighbor = gtopo.GetNeighborRank(nbs[1]);
                  else
                     neighbor = gtopo.GetNeighborRank(nbs[0]);
                  MPI_Isend(face_splittings[i], faces_in_group, MPI_INT,
                            neighbor, 0, MyComm, &request);
               }
            }

            // (b) receive the type of interface splitting
            for (i = 0; i < GetNGroups()-1; i++)
            {
               group_sface.GetRow(i, group_faces);
               faces_in_group = group_faces.Size();
               if (faces_in_group != 0)
               {
                  const int *nbs = gtopo.GetGroup(i+1);
                  if (nbs[0] == 0)
                     neighbor = gtopo.GetNeighborRank(nbs[1]);
                  else
                     neighbor = gtopo.GetNeighborRank(nbs[0]);
                  MPI_Recv(iBuf, faces_in_group, MPI_INT, neighbor,
                           MPI_ANY_TAG, MyComm, &status);

                  for (j = 0; j < faces_in_group; j++)
                     if (iBuf[j] != face_splittings[i][j])
                     {
                        int *v = shared_faces[group_faces[j]]->GetVertices();
                        for (int k = 0; k < 3; k++)
                           if (refined_edge[iBuf[j]][k] == 1 &&
                               refined_edge[face_splittings[i][j]][k] == 0)
                           {
                              int ii = v_to_v(v[k], v[(k+1)%3]);
                              if (middle[ii] == -1)
                              {
                                 need_refinement = 1;
                                 middle[ii] = NumOfVertices++;
                                 for (int c = 0; c < 3; c++)
                                    V(c) = 0.5 * (vertices[v[k]](c) +
                                                  vertices[v[(k+1)%3]](c));
                                 vertices.Append(V);
                              }
                           }
                     }
               }
            }

            i = need_refinement;
            MPI_Allreduce(&i, &need_refinement, 1, MPI_INT, MPI_LOR, MyComm);
         }
      }
      while (need_refinement == 1);

#ifdef MFEM_DEBUG
      i = ref_loops_all;
      MPI_Reduce(&i, &ref_loops_all, 1, MPI_INT, MPI_MAX, 0, MyComm);
      if (MyRank == 0)
      {
         dbg << "\n\nParMesh::LocalRefinement : max. ref_loops_all = "
              << ref_loops_all << ", ref_loops_par = " << ref_loops_par
              << '\n' << endl;
      }
#endif

      delete [] iBuf;
      for (i = 0; i < GetNGroups()-1; i++)
         delete [] face_splittings[i];
      delete [] face_splittings;


      // 5. Update the boundary elements.
      do
      {
         need_refinement = 0;
         for (i = 0; i < NumOfBdrElements; i++)
            if (boundary[i]->NeedRefinement(v_to_v, middle))
            {
               need_refinement = 1;
               Bisection(i, v_to_v, middle);
            }
      }
      while (need_refinement == 1);

      if (NumOfBdrElements != boundary.Size())
         mfem_error("ParMesh::LocalRefinement :"
                    " (NumOfBdrElements != boundary.Size())");

      // 5a. Update the groups after refinement.
      if (el_to_face != NULL)
      {
         if (WantTwoLevelState)
         {
            c_el_to_face = el_to_face;
            el_to_face = NULL;
            Swap(faces_info, fc_faces_info);
         }
         RefineGroups(v_to_v, middle);
         // GetElementToFaceTable(); // Called by RefineGroups
         GenerateFaces();
         if (WantTwoLevelState)
         {
            f_el_to_face = el_to_face;
         }
      }

      // 6. Un-mark the Pf elements.
      int refinement_edges[2], type, flag;
      for (i = 0; i < NumOfElements; i++)
      {
         Element *El = elements[i];
         while (El->GetType() == Element::BISECTED)
            El = ((BisectedElement *) El)->FirstChild;
         ((Tetrahedron *) El)->ParseRefinementFlag(refinement_edges,
                                                   type, flag);
         if (type == Tetrahedron::TYPE_PF)
            ((Tetrahedron *) El)->CreateRefinementFlag(refinement_edges,
                                                       Tetrahedron::TYPE_PU,
                                                       flag);
      }

      // 7. Free the allocated memory.
      middle.DeleteAll();

#ifdef MFEM_DEBUG
      CheckElementOrientation();
#endif

      if (el_to_edge != NULL)
      {
         if (WantTwoLevelState)
         {
            c_el_to_edge = el_to_edge;
            f_el_to_edge = new Table;
            c_bel_to_edge = bel_to_edge;
            bel_to_edge = NULL;
            NumOfEdges = GetElementToEdgeTable(*f_el_to_edge, be_to_edge);
            el_to_edge = f_el_to_edge;
            f_bel_to_edge = bel_to_edge;
         }
         else
            NumOfEdges = GetElementToEdgeTable(*el_to_edge, be_to_edge);
      }

      if (WantTwoLevelState)
      {
         f_NumOfVertices    = NumOfVertices;
         f_NumOfEdges       = NumOfEdges;
         f_NumOfFaces       = NumOfFaces;
         f_NumOfElements    = NumOfElements;
         f_NumOfBdrElements = NumOfBdrElements;
      }
   } //  'if (Dim == 3)'


   if (Dim == 2)
   {
      if (WantTwoLevelState)
      {
         c_NumOfVertices    = NumOfVertices;
         c_NumOfEdges       = NumOfEdges;
         c_NumOfElements    = NumOfElements;
         c_NumOfBdrElements = NumOfBdrElements;
      }

      int uniform_refinement = 0;
      if (type < 0)
      {
         type = -type;
         uniform_refinement = 1;
      }

      // 1. Get table of vertex to vertex connections.
      DSTable v_to_v(NumOfVertices);
      GetVertexToVertexTable(v_to_v);

      // 2. Get edge to element connections in arrays edge1 and edge2
      int nedges  = v_to_v.NumberOfEntries();
      int *edge1  = new int[nedges];
      int *edge2  = new int[nedges];
      int *middle = new int[nedges];

      for (i = 0; i < nedges; i++)
         edge1[i] = edge2[i] = middle[i] = -1;

      for (i = 0; i < NumOfElements; i++)
      {
         int *v = elements[i]->GetVertices();
         for (j = 0; j < 3; j++)
         {
            int ind = v_to_v(v[j], v[(j+1)%3]);
            (edge1[ind] == -1) ? (edge1[ind] = i) : (edge2[ind] = i);
         }
      }

      // 3. Do the red refinement.
      for (i = 0; i < marked_el.Size(); i++)
         RedRefinement(marked_el[i], v_to_v, edge1, edge2, middle);

      if (WantTwoLevelState)
      {
         RefinedElement::State = RefinedElement::FINE;
         State = Mesh::TWO_LEVEL_FINE;
      }

      // 4. Do the green refinement (to get conforming mesh).
      int need_refinement;
      int edges_in_group, max_edges_in_group = 0;
      // edge_splittings identify how the shared edges have been split
      int **edge_splittings = new int*[GetNGroups()-1];
      for (i = 0; i < GetNGroups()-1; i++)
      {
         edges_in_group = GroupNEdges(i+1);
         edge_splittings[i] = new int[edges_in_group];
         if (edges_in_group > max_edges_in_group)
            max_edges_in_group = edges_in_group;
      }
      int neighbor, *iBuf = new int[max_edges_in_group];

      Array<int> group_edges;

      MPI_Request request;
      MPI_Status  status;
      Vertex V;
      V(2) = 0.0;

#ifdef MFEM_DEBUG
      int ref_loops_all = 0, ref_loops_par = 0;
#endif
      do
      {
         need_refinement = 0;
         for (i = 0; i < nedges; i++)
            if (middle[i] != -1 && edge1[i] != -1)
            {
               need_refinement = 1;
               GreenRefinement(edge1[i], v_to_v, edge1, edge2, middle);
            }
#ifdef MFEM_DEBUG
         ref_loops_all++;
#endif

         if (uniform_refinement)
            continue;

         // if the mesh is locally conforming start making it globally
         // conforming
         if (need_refinement == 0)
         {
#ifdef MFEM_DEBUG
            ref_loops_par++;
#endif
            // MPI_Barrier(MyComm);

            // (a) send the type of interface splitting
            for (i = 0; i < GetNGroups()-1; i++)
            {
               group_sedge.GetRow(i, group_edges);
               edges_in_group = group_edges.Size();
               // it is enough to communicate through the edges
               if (edges_in_group != 0)
               {
                  for (j = 0; j < edges_in_group; j++)
                     edge_splittings[i][j] =
                        GetEdgeSplittings(shared_edges[group_edges[j]], v_to_v,
                                          middle);
                  const int *nbs = gtopo.GetGroup(i+1);
                  if (nbs[0] == 0)
                     neighbor = gtopo.GetNeighborRank(nbs[1]);
                  else
                     neighbor = gtopo.GetNeighborRank(nbs[0]);
                  MPI_Isend(edge_splittings[i], edges_in_group, MPI_INT,
                            neighbor, 0, MyComm, &request);
               }
            }

            // (b) receive the type of interface splitting
            for (i = 0; i < GetNGroups()-1; i++)
            {
               group_sedge.GetRow(i, group_edges);
               edges_in_group = group_edges.Size();
               if (edges_in_group != 0)
               {
                  const int *nbs = gtopo.GetGroup(i+1);
                  if (nbs[0] == 0)
                     neighbor = gtopo.GetNeighborRank(nbs[1]);
                  else
                     neighbor = gtopo.GetNeighborRank(nbs[0]);
                  MPI_Recv(iBuf, edges_in_group, MPI_INT, neighbor,
                           MPI_ANY_TAG, MyComm, &status);

                  for (j = 0; j < edges_in_group; j++)
                     if (iBuf[j] == 1 && edge_splittings[i][j] == 0)
                     {
                        int *v = shared_edges[group_edges[j]]->GetVertices();
                        int ii = v_to_v(v[0], v[1]);
#ifdef MFEM_DEBUG
                        if (middle[ii] != -1)
                           mfem_error("ParMesh::LocalRefinement (triangles) : "
                                      "Oops!");
#endif
                        need_refinement = 1;
                        middle[ii] = NumOfVertices++;
                        for (int c = 0; c < 2; c++)
                           V(c) = 0.5 * (vertices[v[0]](c) + vertices[v[1]](c));
                        vertices.Append(V);
                     }
               }
            }

            i = need_refinement;
            MPI_Allreduce(&i, &need_refinement, 1, MPI_INT, MPI_LOR, MyComm);
         }
      }
      while (need_refinement == 1);

#ifdef MFEM_DEBUG
      i = ref_loops_all;
      MPI_Reduce(&i, &ref_loops_all, 1, MPI_INT, MPI_MAX, 0, MyComm);
      if (MyRank == 0)
      {
         dbg << "\n\nParMesh::LocalRefinement : max. ref_loops_all = "
              << ref_loops_all << ", ref_loops_par = " << ref_loops_par
              << '\n' << endl;
      }
#endif

      for (i = 0; i < GetNGroups()-1; i++)
         delete [] edge_splittings[i];
      delete [] edge_splittings;

      delete [] iBuf;

      // 5. Update the boundary elements.
      int v1[2], v2[2], bisect, temp;
      temp = NumOfBdrElements;
      for (i = 0; i < temp; i++)
      {
         int *v = boundary[i]->GetVertices();
         bisect = v_to_v(v[0], v[1]);
         if (middle[bisect] != -1)
         {  // the element was refined (needs updating)
            if (boundary[i]->GetType() == Element::SEGMENT)
            {
               v1[0] =           v[0]; v1[1] = middle[bisect];
               v2[0] = middle[bisect]; v2[1] =           v[1];

               if (WantTwoLevelState)
               {
                  boundary.Append(new Segment(v2, boundary[i]->GetAttribute()));
#ifdef MFEM_USE_MEMALLOC
                  BisectedElement *aux = BEMemory.Alloc();
                  aux->SetCoarseElem(boundary[i]);
#else
                  BisectedElement *aux = new BisectedElement(boundary[i]);
#endif
                  aux->FirstChild =
                     new Segment(v1, boundary[i]->GetAttribute());
                  aux->SecondChild = NumOfBdrElements;
                  boundary[i] = aux;
                  NumOfBdrElements++;
               }
               else
               {
                  boundary[i]->SetVertices(v1);
                  boundary.Append(new Segment(v2, boundary[i]->GetAttribute()));
               }
            }
            else
               mfem_error("Only bisection of segment is implemented for bdr"
                          " elem.");
         }
      }
      NumOfBdrElements = boundary.Size();

      // 5a. Update the groups after refinement.
      RefineGroups(v_to_v, middle);

      // 6. Free the allocated memory.
      delete [] edge1;
      delete [] edge2;
      delete [] middle;

#ifdef MFEM_DEBUG
      CheckElementOrientation();
#endif

      if (WantTwoLevelState)
      {
         f_NumOfVertices    = NumOfVertices;
         f_NumOfElements    = NumOfElements;
         f_NumOfBdrElements = NumOfBdrElements;
         RefinedElement::State = RefinedElement::FINE;
         State = Mesh::TWO_LEVEL_FINE;
      }

      if (el_to_edge != NULL)
      {
         if (WantTwoLevelState)
         {
            c_el_to_edge = el_to_edge;
            Swap(be_to_edge, fc_be_to_edge); // save coarse be_to_edge
            f_el_to_edge = new Table;
            NumOfEdges = GetElementToEdgeTable(*f_el_to_edge, be_to_edge);
            el_to_edge = f_el_to_edge;
            f_NumOfEdges = NumOfEdges;
         }
         else
            NumOfEdges = GetElementToEdgeTable(*el_to_edge, be_to_edge);
         GenerateFaces();
      }
   } //  'if (Dim == 2)'

   if (Nodes)  // curved mesh
   {
      UpdateNodes();
      UseTwoLevelState(wtls);
   }
}

void ParMesh::RefineGroups(const DSTable &v_to_v, int *middle)
{
   int i, attr, newv[3], ind, f_ind, *v;

   int group;
   Array<int> group_verts, group_edges, group_faces;

   // To update the groups after a refinement, we observe that:
   // - every (new and old) vertex, edge and face belongs to exactly one group
   // - the refinement does not create new groups
   // - a new vertex appears only as the middle of a refined edge
   // - a face can be refined 2, 3 or 4 times producing new edges and faces

   int *I_group_svert, *J_group_svert;
   int *I_group_sedge, *J_group_sedge;
   int *I_group_sface, *J_group_sface;

   I_group_svert = new int[GetNGroups()+1];
   I_group_sedge = new int[GetNGroups()+1];
   if (Dim == 3)
      I_group_sface = new int[GetNGroups()+1];

   I_group_svert[0] = I_group_svert[1] = 0;
   I_group_sedge[0] = I_group_sedge[1] = 0;
   if (Dim == 3)
      I_group_sface[0] = I_group_sface[1] = 0;

   // overestimate the size of the J arrays
   if (Dim == 3)
   {
      J_group_svert = new int[group_svert.Size_of_connections()
                              + group_sedge.Size_of_connections()];
      J_group_sedge = new int[2*group_sedge.Size_of_connections()
                              + 3*group_sface.Size_of_connections()];
      J_group_sface = new int[4*group_sface.Size_of_connections()];
   }
   else if (Dim == 2)
   {
      J_group_svert = new int[group_svert.Size_of_connections()
                              + group_sedge.Size_of_connections()];
      J_group_sedge = new int[2*group_sedge.Size_of_connections()];
   }

   for (group = 0; group < GetNGroups()-1; group++)
   {
      // Get the group shared objects
      group_svert.GetRow(group, group_verts);
      group_sedge.GetRow(group, group_edges);
      group_sface.GetRow(group, group_faces);

      // Check which edges have been refined
      for (i = 0; i < group_sedge.RowSize(group); i++)
      {
         v = shared_edges[group_edges[i]]->GetVertices();
         ind = middle[v_to_v(v[0], v[1])];
         if (ind != -1)
         {
            // add a vertex
            group_verts.Append(svert_lvert.Append(ind)-1);
            // update the edges
            attr = shared_edges[group_edges[i]]->GetAttribute();
            shared_edges.Append(new Segment(v[1], ind, attr));
            group_edges.Append(sedge_ledge.Append(-1)-1);
            v[1] = ind;
         }
      }

      // Check which faces have been refined
      for (i = 0; i < group_sface.RowSize(group); i++)
      {
         v = shared_faces[group_faces[i]]->GetVertices();
         ind = middle[v_to_v(v[0], v[1])];
         if (ind != -1)
         {
            attr = shared_faces[group_faces[i]]->GetAttribute();
            // add the refinement edge
            shared_edges.Append(new Segment(v[2], ind, attr));
            group_edges.Append(sedge_ledge.Append(-1)-1);
            // add a face
            f_ind = group_faces.Size();
            shared_faces.Append(new Triangle(v[1], v[2], ind, attr));
            group_faces.Append(sface_lface.Append(-1)-1);
            newv[0] = v[2]; newv[1] = v[0]; newv[2] = ind;
            shared_faces[group_faces[i]]->SetVertices(newv);

            // check if the left face has also been refined
            // v = shared_faces[group_faces[i]]->GetVertices();
            ind = middle[v_to_v(v[0], v[1])];
            if (ind != -1)
            {
               // add the refinement edge
               shared_edges.Append(new Segment(v[2], ind, attr));
               group_edges.Append(sedge_ledge.Append(-1)-1);
               // add a face
               shared_faces.Append(new Triangle(v[1], v[2], ind, attr));
               group_faces.Append(sface_lface.Append(-1)-1);
               newv[0] = v[2]; newv[1] = v[0]; newv[2] = ind;
               shared_faces[group_faces[i]]->SetVertices(newv);
            }

            // check if the right face has also been refined
            v = shared_faces[group_faces[f_ind]]->GetVertices();
            ind = middle[v_to_v(v[0], v[1])];
            if (ind != -1)
            {
               // add the refinement edge
               shared_edges.Append(new Segment(v[2], ind, attr));
               group_edges.Append(sedge_ledge.Append(-1)-1);
               // add a face
               shared_faces.Append(new Triangle(v[1], v[2], ind, attr));
               group_faces.Append(sface_lface.Append(-1)-1);
               newv[0] = v[2]; newv[1] = v[0]; newv[2] = ind;
               shared_faces[group_faces[f_ind]]->SetVertices(newv);
            }
         }
      }

      I_group_svert[group+1] = I_group_svert[group] + group_verts.Size();
      I_group_sedge[group+1] = I_group_sedge[group] + group_edges.Size();
      if (Dim == 3)
         I_group_sface[group+1] = I_group_sface[group] + group_faces.Size();

      int *J;
      J = J_group_svert+I_group_svert[group];
      for (i = 0; i < group_verts.Size(); i++)
         J[i] = group_verts[i];
      J = J_group_sedge+I_group_sedge[group];
      for (i = 0; i < group_edges.Size(); i++)
         J[i] = group_edges[i];
      if (Dim == 3)
      {
         J = J_group_sface+I_group_sface[group];
         for (i = 0; i < group_faces.Size(); i++)
            J[i] = group_faces[i];
      }
   }

   // Fix the local numbers of shared edges and faces
   {
      DSTable new_v_to_v(NumOfVertices);
      GetVertexToVertexTable(new_v_to_v);
      for (i = 0; i < shared_edges.Size(); i++)
      {
         v = shared_edges[i]->GetVertices();
         sedge_ledge[i] = new_v_to_v(v[0], v[1]);
      }
   }
   if (Dim == 3)
   {
      STable3D *faces_tbl = GetElementToFaceTable(1);
      for (i = 0; i < shared_faces.Size(); i++)
      {
         v = shared_faces[i]->GetVertices();
         sface_lface[i] = (*faces_tbl)(v[0], v[1], v[2]);
      }
      delete faces_tbl;
   }

   group_svert.SetIJ(I_group_svert, J_group_svert);
   group_sedge.SetIJ(I_group_sedge, J_group_sedge);
   if (Dim == 3)
      group_sface.SetIJ(I_group_sface, J_group_sface);
}

void ParMesh::QuadUniformRefinement()
{
   SetState(Mesh::NORMAL);

   int oedge = NumOfVertices, wtls = WantTwoLevelState;

   if (Nodes)  // curved mesh
      UseTwoLevelState(1);

   // call Mesh::QuadUniformRefinement so that it won't update the nodes
   {
      GridFunction *nodes = Nodes;
      Nodes = NULL;
      Mesh::QuadUniformRefinement();
      Nodes = nodes;
   }

   // update the groups
   {
      int i, attr, ind, *v;

      int group;
      Array<int> sverts, sedges;

      int *I_group_svert, *J_group_svert;
      int *I_group_sedge, *J_group_sedge;

      I_group_svert = new int[GetNGroups()+1];
      I_group_sedge = new int[GetNGroups()+1];

      I_group_svert[0] = I_group_svert[1] = 0;
      I_group_sedge[0] = I_group_sedge[1] = 0;

      // compute the size of the J arrays
      J_group_svert = new int[group_svert.Size_of_connections()
                              + group_sedge.Size_of_connections()];
      J_group_sedge = new int[2*group_sedge.Size_of_connections()];

      for (group = 0; group < GetNGroups()-1; group++)
      {
         // Get the group shared objects
         group_svert.GetRow(group, sverts);
         group_sedge.GetRow(group, sedges);

         // Process all the edges
         for (i = 0; i < group_sedge.RowSize(group); i++)
         {
            v = shared_edges[sedges[i]]->GetVertices();
            ind = oedge + sedge_ledge[sedges[i]];
            // add a vertex
            sverts.Append(svert_lvert.Append(ind)-1);
            // update the edges
            attr = shared_edges[sedges[i]]->GetAttribute();
            shared_edges.Append(new Segment(v[1], ind, attr));
            sedges.Append(sedge_ledge.Append(-1)-1);
            v[1] = ind;
         }

         I_group_svert[group+1] = I_group_svert[group] + sverts.Size();
         I_group_sedge[group+1] = I_group_sedge[group] + sedges.Size();

         int *J;
         J = J_group_svert+I_group_svert[group];
         for (i = 0; i < sverts.Size(); i++)
            J[i] = sverts[i];
         J = J_group_sedge+I_group_sedge[group];
         for (i = 0; i < sedges.Size(); i++)
            J[i] = sedges[i];
      }

      // Fix the local numbers of shared edges
      DSTable v_to_v(NumOfVertices);
      GetVertexToVertexTable(v_to_v);
      for (i = 0; i < shared_edges.Size(); i++)
      {
         v = shared_edges[i]->GetVertices();
         sedge_ledge[i] = v_to_v(v[0], v[1]);
      }

      group_svert.SetIJ(I_group_svert, J_group_svert);
      group_sedge.SetIJ(I_group_sedge, J_group_sedge);
   }

   if (Nodes)  // curved mesh
   {
      UpdateNodes();
      UseTwoLevelState(wtls);
   }
}

void ParMesh::HexUniformRefinement()
{
   SetState(Mesh::NORMAL);

   int wtls = WantTwoLevelState;
   int oedge = NumOfVertices;
   int oface = oedge + NumOfEdges;

   DSTable v_to_v(NumOfVertices);
   GetVertexToVertexTable(v_to_v);
   STable3D *faces_tbl = GetFacesTable();

   if (Nodes)  // curved mesh
      UseTwoLevelState(1);

   // call Mesh::HexUniformRefinement so that it won't update the nodes
   {
      GridFunction *nodes = Nodes;
      Nodes = NULL;
      Mesh::HexUniformRefinement();
      Nodes = nodes;
   }

   // update the groups
   {
      int i, attr, newv[4], ind, m[5];
      Array<int> v;

      int group;
      Array<int> group_verts, group_edges, group_faces;

      int *I_group_svert, *J_group_svert;
      int *I_group_sedge, *J_group_sedge;
      int *I_group_sface, *J_group_sface;

      I_group_svert = new int[GetNGroups()+1];
      I_group_sedge = new int[GetNGroups()+1];
      I_group_sface = new int[GetNGroups()+1];

      I_group_svert[0] = I_group_svert[1] = 0;
      I_group_sedge[0] = I_group_sedge[1] = 0;
      I_group_sface[0] = I_group_sface[1] = 0;

      // compute the size of the J arrays
      J_group_svert = new int[group_svert.Size_of_connections()
                              + group_sedge.Size_of_connections()
                              + group_sface.Size_of_connections()];
      J_group_sedge = new int[2*group_sedge.Size_of_connections()
                              + 4*group_sface.Size_of_connections()];
      J_group_sface = new int[4*group_sface.Size_of_connections()];

      for (group = 0; group < GetNGroups()-1; group++)
      {
         // Get the group shared objects
         group_svert.GetRow(group, group_verts);
         group_sedge.GetRow(group, group_edges);
         group_sface.GetRow(group, group_faces);

         // Process the edges that have been refined
         for (i = 0; i < group_sedge.RowSize(group); i++)
         {
            shared_edges[group_edges[i]]->GetVertices(v);
            ind = oedge + v_to_v(v[0], v[1]);
            // add a vertex
            group_verts.Append(svert_lvert.Append(ind)-1);
            // update the edges
            attr = shared_edges[group_edges[i]]->GetAttribute();
            shared_edges.Append(new Segment(v[1], ind, attr));
            group_edges.Append(sedge_ledge.Append(-1)-1);
            newv[0] = v[0]; newv[1] = ind;
            shared_edges[group_edges[i]]->SetVertices(newv);
         }

         // Process the faces that have been refined
         for (i = 0; i < group_sface.RowSize(group); i++)
         {
            shared_faces[group_faces[i]]->GetVertices(v);
            m[0] = oface+(*faces_tbl)(v[0], v[1], v[2], v[3]);
            // add a vertex
            group_verts.Append(svert_lvert.Append(m[0])-1);
            // add the refinement edges
            attr = shared_faces[group_faces[i]]->GetAttribute();
            m[1] = oedge + v_to_v(v[0], v[1]);
            m[2] = oedge + v_to_v(v[1], v[2]);
            m[3] = oedge + v_to_v(v[2], v[3]);
            m[4] = oedge + v_to_v(v[3], v[0]);
            shared_edges.Append(new Segment(m[1], m[0], attr));
            group_edges.Append(sedge_ledge.Append(-1)-1);
            shared_edges.Append(new Segment(m[2], m[0], attr));
            group_edges.Append(sedge_ledge.Append(-1)-1);
            shared_edges.Append(new Segment(m[3], m[0], attr));
            group_edges.Append(sedge_ledge.Append(-1)-1);
            shared_edges.Append(new Segment(m[4], m[0], attr));
            group_edges.Append(sedge_ledge.Append(-1)-1);
            // update faces
            newv[0] = v[0]; newv[1] = m[1]; newv[2] = m[0]; newv[3] = m[4];
            shared_faces[group_faces[i]]->SetVertices(newv);
            shared_faces.Append(new Quadrilateral(m[1],v[1],m[2],m[0],attr));
            group_faces.Append(sface_lface.Append(-1)-1);
            shared_faces.Append(new Quadrilateral(m[0],m[2],v[2],m[3],attr));
            group_faces.Append(sface_lface.Append(-1)-1);
            shared_faces.Append(new Quadrilateral(m[4],m[0],m[3],v[3],attr));
            group_faces.Append(sface_lface.Append(-1)-1);
         }

         I_group_svert[group+1] = I_group_svert[group] + group_verts.Size();
         I_group_sedge[group+1] = I_group_sedge[group] + group_edges.Size();
         I_group_sface[group+1] = I_group_sface[group] + group_faces.Size();

         int *J;
         J = J_group_svert+I_group_svert[group];
         for (i = 0; i < group_verts.Size(); i++)
            J[i] = group_verts[i];
         J = J_group_sedge+I_group_sedge[group];
         for (i = 0; i < group_edges.Size(); i++)
            J[i] = group_edges[i];
         J = J_group_sface+I_group_sface[group];
         for (i = 0; i < group_faces.Size(); i++)
            J[i] = group_faces[i];
      }

      // Fix the local numbers of shared edges and faces
      DSTable new_v_to_v(NumOfVertices);
      GetVertexToVertexTable(new_v_to_v);
      for (i = 0; i < shared_edges.Size(); i++)
      {
         shared_edges[i]->GetVertices(v);
         sedge_ledge[i] = new_v_to_v(v[0], v[1]);
      }

      delete faces_tbl;
      faces_tbl = GetFacesTable();
      for (i = 0; i < shared_faces.Size(); i++)
      {
         shared_faces[i]->GetVertices(v);
         sface_lface[i] = (*faces_tbl)(v[0], v[1], v[2], v[3]);
      }
      delete faces_tbl;

      group_svert.SetIJ(I_group_svert, J_group_svert);
      group_sedge.SetIJ(I_group_sedge, J_group_sedge);
      group_sface.SetIJ(I_group_sface, J_group_sface);
   }

   if (Nodes)  // curved mesh
   {
      UpdateNodes();
      UseTwoLevelState(wtls);
   }
}

void ParMesh::NURBSUniformRefinement()
{
   if (MyRank == 0)
      dbg << "\nParMesh::NURBSUniformRefinement : Not supported yet!\n";
}

void ParMesh::PrintXG(ostream &out) const
{
   if (Dim == 3 && meshgen == 1)
   {
      int i, j, nv;
      const int *ind;

      out << "NETGEN_Neutral_Format\n";
      // print the vertices
      out << NumOfVertices << '\n';
      for (i = 0; i < NumOfVertices; i++)
      {
         for (j = 0; j < Dim; j++)
            out << " " << vertices[i](j);
         out << '\n';
      }

      // print the elements
      out << NumOfElements << '\n';
      for (i = 0; i < NumOfElements; i++)
      {
         nv = elements[i]->GetNVertices();
         ind = elements[i]->GetVertices();
         out << elements[i]->GetAttribute();
         for (j = 0; j < nv; j++)
            out << " " << ind[j]+1;
         out << '\n';
      }

      // print the boundary + shared faces information
      out << NumOfBdrElements + shared_faces.Size() << '\n';
      // boundary
      for (i = 0; i < NumOfBdrElements; i++)
      {
         nv = boundary[i]->GetNVertices();
         ind = boundary[i]->GetVertices();
         out << boundary[i]->GetAttribute();
         for (j = 0; j < nv; j++)
            out << " " << ind[j]+1;
         out << '\n';
      }
      // shared faces
      for (i = 0; i < shared_faces.Size(); i++)
      {
         nv = shared_faces[i]->GetNVertices();
         ind = shared_faces[i]->GetVertices();
         out << shared_faces[i]->GetAttribute();
         for (j = 0; j < nv; j++)
            out << " " << ind[j]+1;
         out << '\n';
      }
   }

   if (Dim == 3 && meshgen == 2)
   {
      int i, j, nv;
      const int *ind;

      out << "TrueGrid\n"
          << "1 " << NumOfVertices << " " << NumOfElements << " 0 0 0 0 0 0 0\n"
          << "0 0 0 1 0 0 0 0 0 0 0\n"
          << "0 0 " << NumOfBdrElements+shared_faces.Size()
          << " 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
          << "0.0 0.0 0.0 0 0 0.0 0.0 0 0.0\n"
          << "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";

      // print the vertices
      for (i = 0; i < NumOfVertices; i++)
         out << i+1 << " 0.0 " << vertices[i](0) << " " << vertices[i](1)
             << " " << vertices[i](2) << " 0.0\n";

      // print the elements
      for (i = 0; i < NumOfElements; i++)
      {
         nv = elements[i]->GetNVertices();
         ind = elements[i]->GetVertices();
         out << i+1 << " " << elements[i]->GetAttribute();
         for (j = 0; j < nv; j++)
            out << " " << ind[j]+1;
         out << '\n';
      }

      // print the boundary information
      for (i = 0; i < NumOfBdrElements; i++)
      {
         nv = boundary[i]->GetNVertices();
         ind = boundary[i]->GetVertices();
         out << boundary[i]->GetAttribute();
         for (j = 0; j < nv; j++)
            out << " " << ind[j]+1;
         out << " 1.0 1.0 1.0 1.0\n";
      }

      // print the shared faces information
      for (i = 0; i < shared_faces.Size(); i++)
      {
         nv = shared_faces[i]->GetNVertices();
         ind = shared_faces[i]->GetVertices();
         out << shared_faces[i]->GetAttribute();
         for (j = 0; j < nv; j++)
            out << " " << ind[j]+1;
         out << " 1.0 1.0 1.0 1.0\n";
      }
   }

   if (Dim == 2)
   {
      int i, j, attr;
      Array<int> v;

      out << "areamesh2\n\n";

      // print the boundary + shared edges information
      out << NumOfBdrElements + shared_edges.Size() << '\n';
      // boundary
      for (i = 0; i < NumOfBdrElements; i++)
      {
         attr = boundary[i]->GetAttribute();
         boundary[i]->GetVertices(v);
         out << attr << "     ";
         for (j = 0; j < v.Size(); j++)
            out << v[j] + 1 << "   ";
         out << '\n';
      }
      // shared edges
      for (i = 0; i < shared_edges.Size(); i++)
      {
         attr = shared_edges[i]->GetAttribute();
         shared_edges[i]->GetVertices(v);
         out << attr << "     ";
         for (j = 0; j < v.Size(); j++)
            out << v[j] + 1 << "   ";
         out << '\n';
      }

      // print the elements
      out << NumOfElements << '\n';
      for (i = 0; i < NumOfElements; i++)
      {
         attr = elements[i]->GetAttribute();
         elements[i]->GetVertices(v);

         out << attr << "   ";
         if ((j = GetElementType(i)) == Element::TRIANGLE)
            out << 3 << "   ";
         else
            if (j == Element::QUADRILATERAL)
               out << 4 << "   ";
            else
               if (j == Element::SEGMENT)
                  out << 2 << "   ";
         for (j = 0; j < v.Size(); j++)
            out << v[j] + 1 << "  ";
         out << '\n';
      }

      // print the vertices
      out << NumOfVertices << '\n';
      for (i = 0; i < NumOfVertices; i++)
      {
         for (j = 0; j < Dim; j++)
            out << vertices[i](j) << " ";
         out << '\n';
      }
   }
}

void ParMesh::Print(ostream &out) const
{
   int i, j, shared_bdr_attr;
   const Array<Element *> &shared_bdr =
      (Dim == 3) ? shared_faces : shared_edges;

   if (NURBSext)
   {
      Mesh::Print(out); // does not print shared boundary
      return;
   }

   out << "MFEM mesh v1.0\n";

   // optional
   out <<
      "\n#\n# MFEM Geometry Types (see mesh/geom.hpp):\n#\n"
      "# POINT       = 0\n"
      "# SEGMENT     = 1\n"
      "# TRIANGLE    = 2\n"
      "# SQUARE      = 3\n"
      "# TETRAHEDRON = 4\n"
      "# CUBE        = 5\n"
      "#\n";

   out << "\ndimension\n" << Dim
       << "\n\nelements\n" << NumOfElements << '\n';
   for (i = 0; i < NumOfElements; i++)
      PrintElement(elements[i], out);

   out << "\nboundary\n" << NumOfBdrElements + shared_bdr.Size() << '\n';
   for (i = 0; i < NumOfBdrElements; i++)
      PrintElement(boundary[i], out);

   shared_bdr_attr = bdr_attributes.Max() + MyRank + 1;
   for (i = 0; i < shared_bdr.Size(); i++)
   {
      shared_bdr[i]->SetAttribute(shared_bdr_attr);
      PrintElement(shared_bdr[i], out);
   }
   out << "\nvertices\n" << NumOfVertices << '\n';
   if (Nodes == NULL)
   {
      out << Dim << '\n';
      for (i = 0; i < NumOfVertices; i++)
      {
         out << vertices[i](0);
         for (j = 1; j < Dim; j++)
            out << ' ' << vertices[i](j);
         out << '\n';
      }
   }
   else
   {
      out << "\nnodes\n";
      Nodes->Save(out);
   }
}

void ParMesh::PrintAsOne(ostream &out)
{
   int i, j, k, p, nv_ne[2], &nv = nv_ne[0], &ne = nv_ne[1], vc;
   const int *v;
   MPI_Status status;
   Array<double> vert;
   Array<int> ints;

   if (MyRank == 0)
   {
      out << "MFEM mesh v1.0\n";

      // optional
      out <<
         "\n#\n# MFEM Geometry Types (see mesh/geom.hpp):\n#\n"
         "# POINT       = 0\n"
         "# SEGMENT     = 1\n"
         "# TRIANGLE    = 2\n"
         "# SQUARE      = 3\n"
         "# TETRAHEDRON = 4\n"
         "# CUBE        = 5\n"
         "#\n";

      out << "\ndimension\n" << Dim;
   }

   nv = NumOfElements;
   MPI_Reduce(&nv, &ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
   if (MyRank == 0)
   {
      out << "\n\nelements\n" << ne << '\n';
      for (i = 0; i < NumOfElements; i++)
      {
         // processor number + 1 as attribute and geometry type
         out << 1 << ' ' << elements[i]->GetGeometryType();
         // vertices
         nv = elements[i]->GetNVertices();
         v  = elements[i]->GetVertices();
         for (j = 0; j < nv; j++)
            out << ' ' << v[j];
         out << '\n';
      }
      vc = NumOfVertices;
      for (p = 1; p < NRanks; p++)
      {
         MPI_Recv(nv_ne, 2, MPI_INT, p, 444, MyComm, &status);
         ints.SetSize(ne);
         MPI_Recv(&ints[0], ne, MPI_INT, p, 445, MyComm, &status);
         for (i = 0; i < ne; )
         {
            // processor number + 1 as attribute and geometry type
            out << p+1 << ' ' << ints[i];
            // vertices
            k = Geometries.GetVertices(ints[i++])->GetNPoints();
            for (j = 0; j < k; j++)
               out << ' ' << vc + ints[i++];
            out << '\n';
         }
         vc += nv;
      }
   }
   else
   {
      // for each element send its geometry type and its vertices
      ne = 0;
      for (i = 0; i < NumOfElements; i++)
         ne += 1 + elements[i]->GetNVertices();
      nv = NumOfVertices;
      MPI_Send(nv_ne, 2, MPI_INT, 0, 444, MyComm);
      ints.SetSize(ne);
      for (i = j = 0; i < NumOfElements; i++)
      {
         ints[j++] = elements[i]->GetGeometryType();
         nv = elements[i]->GetNVertices();
         v  = elements[i]->GetVertices();
         for (k = 0; k < nv; k++)
            ints[j++] = v[k];
      }
      MPI_Send(&ints[0], ne, MPI_INT, 0, 445, MyComm);
   }

   // boundary + shared boundary
   Array<Element *> &shared_boundary =
      (Dim == 2) ? shared_edges : shared_faces;
   nv = NumOfBdrElements + shared_boundary.Size();
   MPI_Reduce(&nv, &ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
   if (MyRank == 0)
   {
      out << "\nboundary\n" << ne << '\n';
      // actual boundary
      for (i = 0; i < NumOfBdrElements; i++)
      {
         // processor number + 1 as bdr. attr. and bdr. geometry type
         out << 1 << ' ' << boundary[i]->GetGeometryType();
         // vertices
         nv = boundary[i]->GetNVertices();
         v  = boundary[i]->GetVertices();
         for (j = 0; j < nv; j++)
            out << ' ' << v[j];
         out << '\n';
      }
      // shared boundary (interface)
      for (i = 0; i < shared_boundary.Size(); i++)
      {
         // processor number + 1 as bdr. attr. and bdr. geometry type
         out << 1 << ' ' << shared_boundary[i]->GetGeometryType();
         // vertices
         nv = shared_boundary[i]->GetNVertices();
         v  = shared_boundary[i]->GetVertices();
         for (j = 0; j < nv; j++)
            out << ' ' << v[j];
         out << '\n';
      }
      vc = NumOfVertices;
      for (p = 1; p < NRanks; p++)
      {
         MPI_Recv(nv_ne, 2, MPI_INT, p, 446, MyComm, &status);
         ints.SetSize(ne);
         MPI_Recv(&ints[0], ne, MPI_INT, p, 447, MyComm, &status);
         for (i = 0; i < ne; )
         {
            // processor number + 1 as bdr. attr. and bdr. geometry type
            out << p+1 << ' ' << ints[i];
            k = Geometries.GetVertices(ints[i++])->GetNPoints();
            // vertices
            for (j = 0; j < k; j++)
               out << ' ' << vc + ints[i++];
            out << '\n';
         }
         vc += nv;
      }
   }
   else
   {
      // for each boundary and shared boundary element send its
      // geometry type and its vertices
      ne = 0;
      for (i = 0; i < NumOfBdrElements; i++)
         ne += 1 + boundary[i]->GetNVertices();
      for (i = 0; i < shared_boundary.Size(); i++)
         ne += 1 + shared_boundary[i]->GetNVertices();
      nv = NumOfVertices;
      MPI_Send(nv_ne, 2, MPI_INT, 0, 446, MyComm);
      ints.SetSize(ne);
      // boundary
      for (i = j = 0; i < NumOfBdrElements; i++)
      {
         ints[j++] = boundary[i]->GetGeometryType();
         nv = boundary[i]->GetNVertices();
         v  = boundary[i]->GetVertices();
         for (k = 0; k < nv; k++)
            ints[j++] = v[k];
      }
      // shared boundary
      for (i = 0; i < shared_boundary.Size(); i++)
      {
         ints[j++] = shared_boundary[i]->GetGeometryType();
         nv = shared_boundary[i]->GetNVertices();
         v  = shared_boundary[i]->GetVertices();
         for (k = 0; k < nv; k++)
            ints[j++] = v[k];
      }
      MPI_Send(&ints[0], ne, MPI_INT, 0, 447, MyComm);
   }

   // vertices / nodes
   MPI_Reduce(&NumOfVertices, &nv, 1, MPI_INT, MPI_SUM, 0, MyComm);
   if (MyRank == 0)
      out << "\nvertices\n" << nv << '\n';
   if (Nodes == NULL)
   {
      if (MyRank == 0)
      {
         out << Dim << '\n';
         for (i = 0; i < NumOfVertices; i++)
         {
            out << vertices[i](0);
            for (j = 1; j < Dim; j++)
               out << ' ' << vertices[i](j);
            out << '\n';
         }
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 448, MyComm, &status);
            vert.SetSize(nv*Dim);
            MPI_Recv(&vert[0], nv*Dim, MPI_DOUBLE, p, 449, MyComm, &status);
            for (i = 0; i < nv; i++)
            {
               out << vert[i*Dim];
               for (j = 1; j < Dim; j++)
                  out << ' ' << vert[i*Dim+j];
               out << '\n';
            }
         }
      }
      else
      {
         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 448, MyComm);
         vert.SetSize(NumOfVertices*Dim);
         for (i = 0; i < NumOfVertices; i++)
            for (j = 0; j < Dim; j++)
               vert[i*Dim+j] = vertices[i](j);
         MPI_Send(&vert[0], NumOfVertices*Dim, MPI_DOUBLE, 0, 449, MyComm);
      }
   }
   else
   {
      if (MyRank == 0)
         out << "\nnodes\n";
      ParGridFunction *pnodes = dynamic_cast<ParGridFunction *>(Nodes);
      if (pnodes)
      {
         pnodes->SaveAsOne(out);
      }
      else
      {
         ParFiniteElementSpace *pfes =
            dynamic_cast<ParFiniteElementSpace *>(Nodes->FESpace());
         if (pfes)
         {
            // create a wrapper ParGridFunction
            ParGridFunction ParNodes(pfes, Nodes);
            ParNodes.SaveAsOne(out);
         }
         else
            mfem_error("ParMesh::PrintAsOne : Nodes have no parallel info!");
      }
   }
}

void ParMesh::PrintAsOneXG(ostream &out)
{
   if (Dim == 3 && meshgen == 1)
   {
      int i, j, k, nv, ne, p;
      const int *ind, *v;
      MPI_Status status;
      Array<double> vert;
      Array<int> ints;

      if (MyRank == 0)
      {
         out << "NETGEN_Neutral_Format\n";
         // print the vertices
         ne = NumOfVertices;
         MPI_Reduce(&ne, &nv, 1, MPI_INT, MPI_SUM, 0, MyComm);
         out << nv << '\n';
         for (i = 0; i < NumOfVertices; i++)
         {
            for (j = 0; j < Dim; j++)
               out << " " << vertices[i](j);
            out << '\n';
         }
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 444, MyComm, &status);
            vert.SetSize(Dim*nv);
            MPI_Recv(&vert[0], Dim*nv, MPI_DOUBLE, p, 445, MyComm, &status);
            for (i = 0; i < nv; i++)
            {
               for (j = 0; j < Dim; j++)
                  out << " " << vert[Dim*i+j];
               out << '\n';
            }
         }

         // print the elements
         nv = NumOfElements;
         MPI_Reduce(&nv, &ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
         out << ne << '\n';
         for (i = 0; i < NumOfElements; i++)
         {
            nv = elements[i]->GetNVertices();
            ind = elements[i]->GetVertices();
            out << 1;
            for (j = 0; j < nv; j++)
               out << " " << ind[j]+1;
            out << '\n';
         }
         k = NumOfVertices;
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 444, MyComm, &status);
            MPI_Recv(&ne, 1, MPI_INT, p, 446, MyComm, &status);
            ints.SetSize(4*ne);
            MPI_Recv(&ints[0], 4*ne, MPI_INT, p, 447, MyComm, &status);
            for (i = 0; i < ne; i++)
            {
               out << p+1;
               for (j = 0; j < 4; j++)
                  out << " " << k+ints[i*4+j]+1;
               out << '\n';
            }
            k += nv;
         }
         // print the boundary + shared faces information
         nv = NumOfBdrElements + shared_faces.Size();
         MPI_Reduce(&nv, &ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
         out << ne << '\n';
         // boundary
         for (i = 0; i < NumOfBdrElements; i++)
         {
            nv = boundary[i]->GetNVertices();
            ind = boundary[i]->GetVertices();
            out << 1;
            for (j = 0; j < nv; j++)
               out << " " << ind[j]+1;
            out << '\n';
         }
         // shared faces
         for (i = 0; i < shared_faces.Size(); i++)
         {
            nv = shared_faces[i]->GetNVertices();
            ind = shared_faces[i]->GetVertices();
            out << 1;
            for (j = 0; j < nv; j++)
               out << " " << ind[j]+1;
            out << '\n';
         }
         k = NumOfVertices;
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 444, MyComm, &status);
            MPI_Recv(&ne, 1, MPI_INT, p, 446, MyComm, &status);
            ints.SetSize(3*ne);
            MPI_Recv(&ints[0], 3*ne, MPI_INT, p, 447, MyComm, &status);
            for (i = 0; i < ne; i++)
            {
               out << p+1;
               for (j = 0; j < 3; j++)
                  out << " " << k+ints[i*3+j]+1;
               out << '\n';
            }
            k += nv;
         }
      }
      else
      {
         ne = NumOfVertices;
         MPI_Reduce(&ne, &nv, 1, MPI_INT, MPI_SUM, 0, MyComm);
         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 444, MyComm);
         vert.SetSize(Dim*NumOfVertices);
         for (i = 0; i < NumOfVertices; i++)
            for (j = 0; j < Dim; j++)
               vert[Dim*i+j] = vertices[i](j);
         MPI_Send(&vert[0], Dim*NumOfVertices, MPI_DOUBLE,
                  0, 445, MyComm);
         // elements
         ne = NumOfElements;
         MPI_Reduce(&ne, &nv, 1, MPI_INT, MPI_SUM, 0, MyComm);
         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 444, MyComm);
         MPI_Send(&NumOfElements, 1, MPI_INT, 0, 446, MyComm);
         ints.SetSize(NumOfElements*4);
         for (i = 0; i < NumOfElements; i++)
         {
            v = elements[i]->GetVertices();
            for (j = 0; j < 4; j++)
               ints[4*i+j] = v[j];
         }
         MPI_Send(&ints[0], 4*NumOfElements, MPI_INT, 0, 447, MyComm);
         // boundary + shared faces
         nv = NumOfBdrElements + shared_faces.Size();
         MPI_Reduce(&nv, &ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 444, MyComm);
         ne = NumOfBdrElements + shared_faces.Size();
         MPI_Send(&ne, 1, MPI_INT, 0, 446, MyComm);
         ints.SetSize(3*ne);
         for (i = 0; i < NumOfBdrElements; i++)
         {
            v = boundary[i]->GetVertices();
            for (j = 0; j < 3; j++)
               ints[3*i+j] = v[j];
         }
         for ( ; i < ne; i++)
         {
            v = shared_faces[i-NumOfBdrElements]->GetVertices();
            for (j = 0; j < 3; j++)
               ints[3*i+j] = v[j];
         }
         MPI_Send(&ints[0], 3*ne, MPI_INT, 0, 447, MyComm);
      }
   }

   if (Dim == 3 && meshgen == 2)
   {
      int i, j, k, nv, ne, p;
      const int *ind, *v;
      MPI_Status status;
      Array<double> vert;
      Array<int> ints;

      int TG_nv, TG_ne, TG_nbe;

      if (MyRank == 0)
      {
         MPI_Reduce(&NumOfVertices, &TG_nv, 1, MPI_INT, MPI_SUM, 0, MyComm);
         MPI_Reduce(&NumOfElements, &TG_ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
         nv = NumOfBdrElements + shared_faces.Size();
         MPI_Reduce(&nv, &TG_nbe, 1, MPI_INT, MPI_SUM, 0, MyComm);

         out << "TrueGrid\n"
             << "1 " << TG_nv << " " << TG_ne << " 0 0 0 0 0 0 0\n"
             << "0 0 0 1 0 0 0 0 0 0 0\n"
             << "0 0 " << TG_nbe << " 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
             << "0.0 0.0 0.0 0 0 0.0 0.0 0 0.0\n"
             << "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";

         // print the vertices
         nv = TG_nv;
         for (i = 0; i < NumOfVertices; i++)
            out << i+1 << " 0.0 " << vertices[i](0) << " " << vertices[i](1)
                << " " << vertices[i](2) << " 0.0\n";
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 444, MyComm, &status);
            vert.SetSize(Dim*nv);
            MPI_Recv(&vert[0], Dim*nv, MPI_DOUBLE, p, 445, MyComm, &status);
            for (i = 0; i < nv; i++)
               out << i+1 << " 0.0 " << vert[Dim*i] << " " << vert[Dim*i+1]
                   << " " << vert[Dim*i+2] << " 0.0\n";
         }

         // print the elements
         ne = TG_ne;
         for (i = 0; i < NumOfElements; i++)
         {
            nv = elements[i]->GetNVertices();
            ind = elements[i]->GetVertices();
            out << i+1 << " " << 1;
            for (j = 0; j < nv; j++)
               out << " " << ind[j]+1;
            out << '\n';
         }
         k = NumOfVertices;
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 444, MyComm, &status);
            MPI_Recv(&ne, 1, MPI_INT, p, 446, MyComm, &status);
            ints.SetSize(8*ne);
            MPI_Recv(&ints[0], 8*ne, MPI_INT, p, 447, MyComm, &status);
            for (i = 0; i < ne; i++)
            {
               out << i+1 << " " << p+1;
               for (j = 0; j < 8; j++)
                  out << " " << k+ints[i*8+j]+1;
               out << '\n';
            }
            k += nv;
         }

         // print the boundary + shared faces information
         ne = TG_nbe;
         // boundary
         for (i = 0; i < NumOfBdrElements; i++)
         {
            nv = boundary[i]->GetNVertices();
            ind = boundary[i]->GetVertices();
            out << 1;
            for (j = 0; j < nv; j++)
               out << " " << ind[j]+1;
            out << " 1.0 1.0 1.0 1.0\n";
         }
         // shared faces
         for (i = 0; i < shared_faces.Size(); i++)
         {
            nv = shared_faces[i]->GetNVertices();
            ind = shared_faces[i]->GetVertices();
            out << 1;
            for (j = 0; j < nv; j++)
               out << " " << ind[j]+1;
            out << " 1.0 1.0 1.0 1.0\n";
         }
         k = NumOfVertices;
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 444, MyComm, &status);
            MPI_Recv(&ne, 1, MPI_INT, p, 446, MyComm, &status);
            ints.SetSize(4*ne);
            MPI_Recv(&ints[0], 4*ne, MPI_INT, p, 447, MyComm, &status);
            for (i = 0; i < ne; i++)
            {
               out << p+1;
               for (j = 0; j < 4; j++)
                  out << " " << k+ints[i*4+j]+1;
               out << " 1.0 1.0 1.0 1.0\n";
            }
            k += nv;
         }
      }
      else
      {
         MPI_Reduce(&NumOfVertices, &TG_nv, 1, MPI_INT, MPI_SUM, 0, MyComm);
         MPI_Reduce(&NumOfElements, &TG_ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
         nv = NumOfBdrElements + shared_faces.Size();
         MPI_Reduce(&nv, &TG_nbe, 1, MPI_INT, MPI_SUM, 0, MyComm);

         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 444, MyComm);
         vert.SetSize(Dim*NumOfVertices);
         for (i = 0; i < NumOfVertices; i++)
            for (j = 0; j < Dim; j++)
               vert[Dim*i+j] = vertices[i](j);
         MPI_Send(&vert[0], Dim*NumOfVertices, MPI_DOUBLE, 0, 445, MyComm);
         // elements
         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 444, MyComm);
         MPI_Send(&NumOfElements, 1, MPI_INT, 0, 446, MyComm);
         ints.SetSize(NumOfElements*8);
         for (i = 0; i < NumOfElements; i++)
         {
            v = elements[i]->GetVertices();
            for (j = 0; j < 8; j++)
               ints[8*i+j] = v[j];
         }
         MPI_Send(&ints[0], 8*NumOfElements, MPI_INT, 0, 447, MyComm);
         // boundary + shared faces
         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 444, MyComm);
         ne = NumOfBdrElements + shared_faces.Size();
         MPI_Send(&ne, 1, MPI_INT, 0, 446, MyComm);
         ints.SetSize(4*ne);
         for (i = 0; i < NumOfBdrElements; i++)
         {
            v = boundary[i]->GetVertices();
            for (j = 0; j < 4; j++)
               ints[4*i+j] = v[j];
         }
         for ( ; i < ne; i++)
         {
            v = shared_faces[i-NumOfBdrElements]->GetVertices();
            for (j = 0; j < 4; j++)
               ints[4*i+j] = v[j];
         }
         MPI_Send(&ints[0], 4*ne, MPI_INT, 0, 447, MyComm);
      }
   }

   if (Dim == 2)
   {
      int i, j, k, attr, nv, ne, p;
      Array<int> v;
      MPI_Status status;
      Array<double> vert;
      Array<int> ints;


      if (MyRank == 0)
      {
         out << "areamesh2\n\n";

         // print the boundary + shared edges information
         nv = NumOfBdrElements + shared_edges.Size();
         MPI_Reduce(&nv, &ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
         out << ne << '\n';
         // boundary
         for (i = 0; i < NumOfBdrElements; i++)
         {
            attr = boundary[i]->GetAttribute();
            boundary[i]->GetVertices(v);
            out << attr << "     ";
            for (j = 0; j < v.Size(); j++)
               out << v[j] + 1 << "   ";
            out << '\n';
         }
         // shared edges
         for (i = 0; i < shared_edges.Size(); i++)
         {
            attr = shared_edges[i]->GetAttribute();
            shared_edges[i]->GetVertices(v);
            out << attr << "     ";
            for (j = 0; j < v.Size(); j++)
               out << v[j] + 1 << "   ";
            out << '\n';
         }
         k = NumOfVertices;
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 444, MyComm, &status);
            MPI_Recv(&ne, 1, MPI_INT, p, 446, MyComm, &status);
            ints.SetSize(2*ne);
            MPI_Recv(&ints[0], 2*ne, MPI_INT, p, 447, MyComm, &status);
            for (i = 0; i < ne; i++)
            {
               out << p+1;
               for (j = 0; j < 2; j++)
                  out << " " << k+ints[i*2+j]+1;
               out << '\n';
            }
            k += nv;
         }

         // print the elements
         nv = NumOfElements;
         MPI_Reduce(&nv, &ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
         out << ne << '\n';
         for (i = 0; i < NumOfElements; i++)
         {
            attr = elements[i]->GetAttribute();
            elements[i]->GetVertices(v);
            out << 1 << "   " << 3 << "   ";
            for (j = 0; j < v.Size(); j++)
               out << v[j] + 1 << "  ";
            out << '\n';
         }
         k = NumOfVertices;
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 444, MyComm, &status);
            MPI_Recv(&ne, 1, MPI_INT, p, 446, MyComm, &status);
            ints.SetSize(3*ne);
            MPI_Recv(&ints[0], 3*ne, MPI_INT, p, 447, MyComm, &status);
            for (i = 0; i < ne; i++)
            {
               out << p+1 << " " << 3;
               for (j = 0; j < 3; j++)
                  out << " " << k+ints[i*3+j]+1;
               out << '\n';
            }
            k += nv;
         }

         // print the vertices
         ne = NumOfVertices;
         MPI_Reduce(&ne, &nv, 1, MPI_INT, MPI_SUM, 0, MyComm);
         out << nv << '\n';
         for (i = 0; i < NumOfVertices; i++)
         {
            for (j = 0; j < Dim; j++)
               out << vertices[i](j) << " ";
            out << '\n';
         }
         for (p = 1; p < NRanks; p++)
         {
            MPI_Recv(&nv, 1, MPI_INT, p, 444, MyComm, &status);
            vert.SetSize(Dim*nv);
            MPI_Recv(&vert[0], Dim*nv, MPI_DOUBLE, p, 445, MyComm, &status);
            for (i = 0; i < nv; i++)
            {
               for (j = 0; j < Dim; j++)
                  out << " " << vert[Dim*i+j];
               out << '\n';
            }
         }
      }
      else
      {
         // boundary + shared faces
         nv = NumOfBdrElements + shared_edges.Size();
         MPI_Reduce(&nv, &ne, 1, MPI_INT, MPI_SUM, 0, MyComm);
         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 444, MyComm);
         ne = NumOfBdrElements + shared_edges.Size();
         MPI_Send(&ne, 1, MPI_INT, 0, 446, MyComm);
         ints.SetSize(2*ne);
         for (i = 0; i < NumOfBdrElements; i++)
         {
            boundary[i]->GetVertices(v);
            for (j = 0; j < 2; j++)
               ints[2*i+j] = v[j];
         }
         for ( ; i < ne; i++)
         {
            shared_edges[i-NumOfBdrElements]->GetVertices(v);
            for (j = 0; j < 2; j++)
               ints[2*i+j] = v[j];
         }
         MPI_Send(&ints[0], 2*ne, MPI_INT, 0, 447, MyComm);
         // elements
         ne = NumOfElements;
         MPI_Reduce(&ne, &nv, 1, MPI_INT, MPI_SUM, 0, MyComm);
         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 444, MyComm);
         MPI_Send(&NumOfElements, 1, MPI_INT, 0, 446, MyComm);
         ints.SetSize(NumOfElements*3);
         for (i = 0; i < NumOfElements; i++)
         {
            elements[i]->GetVertices(v);
            for (j = 0; j < 3; j++)
               ints[3*i+j] = v[j];
         }
         MPI_Send(&ints[0], 3*NumOfElements, MPI_INT, 0, 447, MyComm);
         // vertices
         ne = NumOfVertices;
         MPI_Reduce(&ne, &nv, 1, MPI_INT, MPI_SUM, 0, MyComm);
         MPI_Send(&NumOfVertices, 1, MPI_INT, 0, 444, MyComm);
         vert.SetSize(Dim*NumOfVertices);
         for (i = 0; i < NumOfVertices; i++)
            for (j = 0; j < Dim; j++)
               vert[Dim*i+j] = vertices[i](j);
         MPI_Send(&vert[0], Dim*NumOfVertices, MPI_DOUBLE,
                  0, 445, MyComm);
      }
   }
}

void ParMesh::PrintInfo(ostream &out)
{
   int i;
   DenseMatrix J(Dim);
   double h_min, h_max, kappa_min, kappa_max, h, kappa;

   if (MyRank == 0)
      out << "Parallel Mesh Stats:" << endl;

   for (i = 0; i < NumOfElements; i++)
   {
      GetElementJacobian(i, J);
      h = pow(fabs(J.Det()), 1.0/double(Dim));
      kappa = J.CalcSingularvalue(0) / J.CalcSingularvalue(Dim-1);
      if (i == 0)
      {
         h_min = h_max = h;
         kappa_min = kappa_max = kappa;
      }
      else
      {
         if (h < h_min)  h_min = h;
         if (h > h_max)  h_max = h;
         if (kappa < kappa_min)  kappa_min = kappa;
         if (kappa > kappa_max)  kappa_max = kappa;
      }
   }

   double gh_min, gh_max, gk_min, gk_max;
   MPI_Reduce(&h_min, &gh_min, 1, MPI_DOUBLE, MPI_MIN, 0, MyComm);
   MPI_Reduce(&h_max, &gh_max, 1, MPI_DOUBLE, MPI_MAX, 0, MyComm);
   MPI_Reduce(&kappa_min, &gk_min, 1, MPI_DOUBLE, MPI_MIN, 0, MyComm);
   MPI_Reduce(&kappa_max, &gk_max, 1, MPI_DOUBLE, MPI_MAX, 0, MyComm);

   int ldata[5]; // vert, edge, face, elem, neighbors;
   int mindata[5], maxdata[5], sumdata[5];

   // count locally owned vertices, edges, and faces
   ldata[0] = GetNV();
   ldata[1] = GetNEdges();
   ldata[2] = GetNFaces();
   ldata[3] = GetNE();
   ldata[4] = gtopo.GetNumNeighbors()-1;
   for (int gr = 1; gr < GetNGroups(); gr++)
      if (!gtopo.IAmMaster(gr)) // we are not the master
      {
         ldata[0] -= group_svert.RowSize(gr-1);
         ldata[1] -= group_sedge.RowSize(gr-1);
         ldata[2] -= group_sface.RowSize(gr-1);
      }

   MPI_Reduce(ldata, mindata, 5, MPI_INT, MPI_MIN, 0, MyComm);
   MPI_Reduce(ldata, sumdata, 5, MPI_INT, MPI_SUM, 0, MyComm); // overflow?
   MPI_Reduce(ldata, maxdata, 5, MPI_INT, MPI_MAX, 0, MyComm);

   if (MyRank == 0)
   {
      out << '\n'
          << "           "
          << setw(12) << "minimum"
          << setw(12) << "average"
          << setw(12) << "maximum"
          << setw(12) << "total" << '\n';
      out << " vertices  "
          << setw(12) << mindata[0]
          << setw(12) << sumdata[0]/NRanks
          << setw(12) << maxdata[0]
          << setw(12) << sumdata[0] << '\n';
      out << " edges     "
          << setw(12) << mindata[1]
          << setw(12) << sumdata[1]/NRanks
          << setw(12) << maxdata[1]
          << setw(12) << sumdata[1] << '\n';
      if (Dim == 3)
         out << " faces     "
             << setw(12) << mindata[2]
             << setw(12) << sumdata[2]/NRanks
             << setw(12) << maxdata[2]
             << setw(12) << sumdata[2] << '\n';
      out << " elements  "
          << setw(12) << mindata[3]
          << setw(12) << sumdata[3]/NRanks
          << setw(12) << maxdata[3]
          << setw(12) << sumdata[3] << '\n';
      out << " neighbors "
          << setw(12) << mindata[4]
          << setw(12) << sumdata[4]/NRanks
          << setw(12) << maxdata[4] << '\n';
      out << '\n'
          << "       "
          << setw(12) << "minimum"
          << setw(12) << "maximum" << '\n';
      out << " h     "
          << setw(12) << gh_min
          << setw(12) << gh_max << '\n';
      out << " kappa "
          << setw(12) << gk_min
          << setw(12) << gk_max << '\n';
   }
}

ParMesh::~ParMesh()
{
   int i;

   for (i = 0; i < shared_faces.Size(); i++)
      FreeElement(shared_faces[i]);
   for (i = 0; i < shared_edges.Size(); i++)
      FreeElement(shared_edges[i]);

   // The Mesh destructor is called automatically
}

#endif
