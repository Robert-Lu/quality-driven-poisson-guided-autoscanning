#include "Algorithm/Poisson.h"

#include "Poisson/Geometry.h"
#include "Poisson/PoissonParam.h"

#include "Poisson/MarchingCubes.h"
#include "Poisson/Octree.h"
#include "Poisson/SparseMatrix.h"
#include "Poisson/Ply.h"
#include "Poisson/MultiGridOctreeData.h"
#include "vcg/complex/trimesh/point_sampling.h"

class BaseSampler
{
public:
  BaseSampler(CMesh* _m){m=_m; uvSpaceFlag = false; qualitySampling=false; tex=0;};
  CMesh *m;
  QImage* tex;
  int texSamplingWidth;
  int texSamplingHeight;
  bool uvSpaceFlag;
  bool qualitySampling;

  void AddVert(const CMesh::VertexType &p) 
  {
    tri::Allocator<CMesh>::AddVertices(*m,1);
    m->vert.back().ImportData(p);
  }

  void AddFace(const CMesh::FaceType &f, CMesh::CoordType p) 
  {
    tri::Allocator<CMesh>::AddVertices(*m,1);
    m->vert.back().P() = f.P(0)*p[0] + f.P(1)*p[1] +f.P(2)*p[2];
    m->vert.back().N() = f.V(0)->N()*p[0] + f.V(1)->N()*p[1] + f.V(2)->N()*p[2];

 /*   if (qualitySampling)	
      m->vert.back().Q() = f.V(0)->Q()*p[0] + f.V(1)->Q()*p[1] + f.V(2)->Q()*p[2];*/
  }
  void AddTextureSample(const CMesh::FaceType &f, const CMesh::CoordType &p, const Point2i &tp, float edgeDist)
  {
    if (edgeDist != .0) return;

    tri::Allocator<CMesh>::AddVertices(*m,1);

    if(uvSpaceFlag) m->vert.back().P() = Point3f(float(tp[0]),float(tp[1]),0); 
    else m->vert.back().P() = f.P(0)*p[0] + f.P(1)*p[1] +f.P(2)*p[2];

    m->vert.back().N() = f.V(0)->N()*p[0] + f.V(1)->N()*p[1] +f.V(2)->N()*p[2];
    if(tex)
    {
      QRgb val;
      // Computing normalized texels position
      int xpos = (int)(tex->width()  * (float(tp[0])/texSamplingWidth)) % tex->width();
      int ypos = (int)(tex->height() * (1.0- float(tp[1])/texSamplingHeight)) % tex->height();

      if (xpos < 0) xpos += tex->width();
      if (ypos < 0) ypos += tex->height();

      val = tex->pixel(xpos,ypos);
      m->vert.back().C().SetRGB(qRed(val),qGreen(val),qBlue(val));
    }

  }
}; // end class BaseSampler



Poisson::Poisson(RichParameterSet* _para)
{
	samples = NULL; original = NULL; iso_points = NULL; slices = NULL;
  field_points = NULL;
	para = _para;
}

Poisson::~Poisson(void)
{
	samples = NULL; original = NULL; iso_points = NULL; slices = NULL;
  field_points = NULL;
}

void Poisson::setInput(DataMgr* pData)
{
	if(!pData->isSamplesEmpty())
	{
    if (para->getBool("Run Poisson On Original"))
    {
      original = pData->getCurrentOriginal();
    }
    else
    {
      original = pData->getCurrentSamples();		
    }

    if (para->getBool("Run Poisson On Samples"))
    {
      original = pData->getCurrentSamples();		
    }
    
    samples = pData->getCurrentSamples();
    iso_points = pData->getCurrentIsoPoints();
    slices = pData->getCurrentSlices();
    
    model = pData->getCurrentModel();

    if (!pData->isNBVGridsEmpty() && global_paraMgr.nbv.getBool("Use Confidence Separation"))
    {
      field_points = pData->getAllNBVGridCenters();

    }
    else
    {
      field_points = pData->getCurrentFieldPoints();
    }
    
	}
	else
	{
		cout << "ERROR: Poisson::setInput: empty!!" << endl;
		return;
	}
}

void Poisson::run()
{
  if (para->getBool("Run Label ISO Points"))
  {
    runLabelISO();
    return;
  }

  if (para->getBool("Run Label Smooth"))
  {
    runIsoSmooth();
    return;
  }

  if (para->getBool("Run Label Boundary Points"))
  {
    runLabelBoundaryPoints();
    return;
  }

  if (para->getBool("Run Compute View Candidates"))
  {
    runComputeViewCandidates();
    return ;
  }

  if (para->getBool("Run Slice"))
  {
    runSlice();
    return;
  }

  if (para->getBool("Run View Candidates Clustering"))
  {
    runViewCandidatesClustering();
    return;
  }

  if (para->getBool("Compute Original Confidence"))
  {
    runComputeOriginalConfidence();
    return;
  }

  if (para->getBool("Compute Sample Confidence"))
  {
    runComputeSampleConfidence();
    return;
  }

  if (para->getBool("Compute ISO Confidence"))
  {
    runComputeIsoConfidence();
    return;
  }

  if (para->getBool("Run Clear Slice"))
  {
    slices->clear();
    cout << "Run Clear Slice" << endl;
    return;
  }

  runPoisson();
}


void Poisson::runLabelISO()
{
  if (!samples || samples->vert.empty())
  {
    return;
  }

  for (int i = 0; i < iso_points->vert.size(); i++)
  {
    CVertex& v = iso_points->vert[i];
    v.is_hole = false;
  }

  double radius_threshold = para->getDouble("CGrid Radius") / 2;

  Timer time;
  time.start("Sample ISOpoints Neighbor Tree!!");
  GlobalFun::computeBallNeighbors(iso_points, samples, 
              radius_threshold, samples->bbox);
  time.end();

  for (int i = 0; i < iso_points->vert.size(); i++)
  {
    CVertex& v = iso_points->vert[i];
    if (v.original_neighbors.size() >= 2)
    {
      v.is_hole = false;
    }
    else
    {
      v.is_hole = true;
    }
  }

  if (global_paraMgr.drawer.getBool("Show Confidence Color"))
  {
    double radius = para->getDouble("CGrid Radius");
    GlobalFun::computeBallNeighbors(iso_points, samples, 
                                    radius_threshold, samples->bbox);

   double radius2 = radius * radius;
   double iradius16 = -4.0 / radius2;

    for (int i = 0; i < iso_points->vn; i++)
    {
      CVertex& v = iso_points->vert[i];
      
      float sum_confidence = 0;
      float sum_w = 0;

      for (int j = 0; j < v.original_neighbors.size(); j++)
      {
        int sample_idx = v.original_neighbors[j];
        CVertex& t = samples->vert[sample_idx];

        float dist2  = (v.P() - t.P()).SquaredNorm();
        float w = exp(dist2*iradius16);

        sum_confidence += t.eigen_confidence  * w;
        sum_w += w;
      }

      if (!v.original_neighbors.empty())
      {
        v.eigen_confidence = (sum_confidence / sum_w);
      }
      
    }
  }
}

void Poisson::runIsoSmooth()
{
  double radius_threshold = para->getDouble("CGrid Radius") / 2;
  double radius2 = radius_threshold * radius_threshold;
  double iradius16 = -4/radius2;

  Timer time;
  time.start("Sample ISOpoints Neighbor Tree!!");
  GlobalFun::computeBallNeighbors(iso_points, NULL, 
             radius_threshold, samples->bbox);
  time.end();

  if (global_paraMgr.drawer.getBool("Show Confidence Color"))
  {
    for (int i = 0; i < iso_points->vert.size(); i++)
    {
      CVertex& v = iso_points->vert[i];
      if (v.neighbors.size() <= 1)
      {
        continue;
      }

      float confidence_sum = 0;
      for(int j = 0; j < v.neighbors.size(); j++)
      {
        CVertex& t = iso_points->vert[v.neighbors[j]];
        confidence_sum += t.eigen_confidence;
      }

      v.eigen_confidence = confidence_sum / v.neighbors.size();
    }

  }
  else
  {
    for (int i = 0; i < iso_points->vert.size(); i++)
    {
      CVertex& v = iso_points->vert[i];

      if (v.neighbors.empty())
      {
        continue;
      }

      double sum = 0;
      double weight_sum = 0;
      for(int j = 0; j < v.neighbors.size(); j++)
      {
        CVertex& t = iso_points->vert[v.neighbors[j]];
        double dist2 = GlobalFun::computeEulerDistSquare(v.P(), t.P());
        if (t.is_hole)
        {
          double w = exp(dist2 * iradius16);
          sum += 1;
          weight_sum += w;
        }
      }

      double avg = sum / v.neighbors.size();
      //double avg = sum / weight_sum;

      if (avg > 0.5)
      {
        v.is_hole = true;
      }
      else
      {
        v.is_hole = false;
      }

    }
  }

}

void Poisson::runLabelBoundaryPoints()
{
  if (!samples || samples->vert.empty())
  {
    return;
  }

  //each time reset the points
  for (int i = 0; i < iso_points->vert.size(); i++)
  {
    CVertex& v = iso_points->vert[i];
    v.is_boundary = false;
  }

  double radius_threshold = para->getDouble("CGrid Radius") / 2;
  GlobalFun::computeBallNeighbors(iso_points, NULL, 
    radius_threshold, samples->bbox);

  for (size_t i = 0; i < iso_points->vert.size(); ++i)
  {
    CVertex& v = iso_points->vert[i];

    if (v.neighbors.size() < 0)
      continue;

    int nb_hole_points = 0;
    int nb_neighbors = v.neighbors.size();
    for (size_t j = 0; j < v.neighbors.size(); ++j)
    {
      CVertex& t = iso_points->vert[v.neighbors[j]];
      if (t.is_hole) ++nb_hole_points;
    }

    double ratio = nb_hole_points * 1.0f / nb_neighbors;

    if (ratio > 0.3 && ratio < 0.6)
      v.is_boundary = true;
  }
}

void Poisson::runComputeViewCandidates()
{
  double view_candidates_dist = para->getDouble("View Candidates Distance");
  int index = 0;
  view_candidates->bbox.SetNull();
  for (size_t i = 0; i < iso_points->vert.size(); ++i)
  {
    CVertex t = iso_points->vert[i];
    if (t.is_boundary)
    {
      t.P() = t.P() + t.N() * view_candidates_dist;
      t.N() = t.N() * (-1);
      t.is_boundary = false;
      t.is_view_candidates = true;
      t.m_index = index++;
      view_candidates->vert.push_back(t);
      view_candidates->bbox.Add(t.P());
    }
  }
  view_candidates->vn = view_candidates->vert.size();
}

void Poisson::runViewCandidatesClustering()
{
  double radius = para->getDouble("CGrid Radius"); 
  double radius2 = radius * radius;
  double iradius16 = -4/radius2;

  double sigma = 45;
  double cos_sigma = cos(sigma / 180.0 * 3.1415926);
  double sharpness_bandwidth = std::pow((std::max)(1e-8, 1 - cos_sigma), 2);

  //GlobalFun::computeBallNeighbors(view_candidates, NULL, 
  //                                radius, view_candidates->bbox);
  GlobalFun::computeAnnNeigbhors(view_candidates->vert,
                                 view_candidates->vert, 
                                 15,
                                 false,
                                 "runViewCandidatesClustering");

  vector<CVertex> update_temp;
  for(int i = 0; i < view_candidates->vert.size(); i++)
  {
    CVertex& v = view_candidates->vert[i];

    //if (v.neighbors.size() <= 5)
    if (v.neighbors.empty())
    {
      //update_temp.push_back(v);
      continue;
    }

    Point3f average_positon = Point3f(0, 0, 0);
    Point3f average_normal = Point3f(0, 0, 0);
    double sum_weight = 0.0;

    for (int j = 0; j < v.neighbors.size(); j++)
    {
       CVertex& t = view_candidates->vert[v.neighbors[j]];

       Point3f diff = v.P() - t.P();
       double dist2  = diff.SquaredNorm();

       double dist_weight = exp(dist2 * iradius16);
       double normal_weight = exp(-std::pow(1 - v.N() * t.N(), 2));
       double weight = dist_weight;

       average_positon += t.P() * weight;
       average_normal += t.N() * weight;
       sum_weight += weight;
    }

    CVertex temp_v = v;
    temp_v.P() = average_positon / sum_weight;
    temp_v.N() = average_normal / sum_weight;
    update_temp.push_back(temp_v);
  }

  view_candidates->vert.clear();
  for (int i = 0; i < update_temp.size(); i++)
  {
    view_candidates->vert.push_back(update_temp[i]);
  }
  view_candidates->vn = view_candidates->vert.size(); 

}

void Poisson::runPoisson() 
{
  vector<Point3D<float> > Pts(original->vn);
  vector<Point3D<float> > Nor(original->vn); 

  for (int i = 0; i < original->vn; i++)
  {
    CVertex& v = original->vert[i];
    for (int ii = 0; ii < 3; ++ii)
    {
      Pts[i].coords[ii] = v.P()[ii];
      Nor[i].coords[ii] = v.N()[ii];
    }
  }
  CoredVectorMeshData<PlyVertex<float>> mesh;
  //CoredVectorMeshData<Point3D<float>> mesh;

  XForm4x4< float > xForm , iXForm;
  xForm = XForm4x4< float >::Identity();
  iXForm = xForm.inverse();

  Timer time;
  time.start("build tree");
  PoissonParam Par;
  Par.Depth = para->getDouble("Max Depth");
  Par.SamplesPerNode = 1;
  Par.SolverDivide = 6;
  Par.Offset = 1;
  Par.Confidence = false;

  Point3D<float> center;
  float scale = 1.0;
  float isoValue=0;

  ////////////////////////////////////
  //// Fix courtesy of David Gallup //
  //TreeNodeData::UseIndex = 1;     //
  ////////////////////////////////////

  //// Execute
  ////int ret= Execute2(pp, Pts, Nor, mesh, center, scale, &cb);
  const int Degree = 2;
  const bool OutputDensity = false;

  POctree<Degree, OutputDensity> tree;
  tree.threads = Par.Threads;
  //PPolynomial<Degree> ReconstructionFunction=PPolynomial<Degree>::GaussianApproximation();

  center.coords[0]=center.coords[1]=center.coords[2]=0;

  //TreeOctNode::SetPAllocator(MEMORY_PALLOCATOR_BLOCK_SIZE);
  OctNode< TreeNodeData< OutputDensity > , Real >::SetAllocator( MEMORY_ALLOCATOR_BLOCK_SIZE );
  
  int kernelDepth = Par.Depth-2;
  if(Par.KernelDepth>=0){kernelDepth=Par.KernelDepth;}

  tree.setBSplineData(Par.Depth , Par.BoundaryType);

  double maxMemoryUsage;
  tree.maxMemoryUsage=0;

    int pointCount = tree.setTree2(Pts, 
                                   Nor,
                                   Par.Depth , 
                                   Par.MinDepth, 
                                   kernelDepth , 
                                   Real(Par.SamplesPerNode) , 
                                   Par.Scale , 
                                   Par.Confidence , 
                                   Par.constraintWeight , 
                                   Par.adaptiveExponent , 
                                   xForm );


  tree.ClipTree();
  tree.finalize( Par.IsoDivide );
  tree.SetLaplacianConstraints();

    tree.LaplacianMatrixIteration( Par.SolverDivide, 
                                   Par.ShowResidual , 
                                   Par.MinIters , 
                                   Par.SolverAccuracy , 
                                   Par.MaxSolveDepth , 
                                   Par.FixedIters );

   isoValue = tree.GetIsoValue();


  double estimate_scale = abs(isoValue);
  global_paraMgr.glarea.setValue("Slice Color Scale", DoubleValue(estimate_scale*2/3));
  global_paraMgr.glarea.setValue("ISO Interval Size", DoubleValue(estimate_scale/3));

  //iso_points->vert.clear();
  if (para->getBool("Run Extract MC Points"))
  {
    time.start("marching cube");
    //if(Par.IsoDivide){tree.GetMCIsoTriangles(isoValue,Par.IsoDivide,&mesh);}
    //else{tree.GetMCIsoTriangles(isoValue,&mesh);}
    tree.GetMCIsoTriangles( isoValue , Par.IsoDivide, &mesh , 0 , 1 , !Par.NonManifold , Par.PolygonMesh);

    time.end();

    ////out put
    mesh.resetIterator();
    int vm = mesh.outOfCorePointCount()+mesh.inCorePoints.size();
    int fm = mesh.polygonCount();

    tentative_mesh.vert.clear();
    tentative_mesh.face.clear();
    //tri::MyAllocator<CMesh>::AddVertices(tentative_mesh,vm);
    //tri::MyAllocator<CMesh>::AddFaces(tentative_mesh,fm);

    Point3D<float> p;
    PlyVertex<float> pv; 
    int i=0;
    int index = 0;
    for (; i < int(mesh.inCorePoints.size()); i++)
    {
      p = mesh.inCorePoints[i].point;
      CVertex new_v;
      new_v.P()[0] = p.coords[0]*scale+center.coords[0];
      new_v.P()[1] = p.coords[1]*scale+center.coords[1];
      new_v.P()[2] = p.coords[2]*scale+center.coords[2];
      new_v.is_iso = true;
      new_v.eigen_confidence = 0;
      new_v.m_index = index++;
      tentative_mesh.vert.push_back(new_v);
      tentative_mesh.bbox.Add(new_v.P());
    }

    for (int ii=0; ii < mesh.outOfCorePointCount(); ii++)
    {
      mesh.nextOutOfCorePoint(pv);
      p = pv.point;
      CVertex new_v;
      new_v.P()[0] = p.coords[0]*scale+center.coords[0];
      new_v.P()[1] = p.coords[1]*scale+center.coords[1];
      new_v.P()[2] = p.coords[2]*scale+center.coords[2];
      new_v.eigen_confidence = 0;
      new_v.is_iso = true;
      new_v.m_index = index++;
      tentative_mesh.vert.push_back(new_v);
      tentative_mesh.bbox.Add(new_v.P());
    }

    //TriangleIndex tIndex;
    std::vector< CoredVertexIndex > polygon;
    int inCoreFlag;
    int nr_faces=mesh.polygonCount();	

    for (i=0; i < nr_faces; i++){
      //
      // create and fill a struct that the ply code can handle
      //
      //if (!mesh.nextPolygon(polygon))
      //{
      //  continue;
      //}
      mesh.nextPolygon(polygon);
      //if(!(inCoreFlag & CoredMeshData::IN_CORE_FLAG[0])){tIndex.idx[0]+=int(mesh.inCorePoints.size());}
      //if(!(inCoreFlag & CoredMeshData::IN_CORE_FLAG[1])){tIndex.idx[1]+=int(mesh.inCorePoints.size());}
      //if(!(inCoreFlag & CoredMeshData::IN_CORE_FLAG[2])){tIndex.idx[2]+=int(mesh.inCorePoints.size());}
      CFace new_face;      
      for(int j=0; j < 3; j++)
      {
        //tentative_mesh.face[i].V(j) = &tentative_mesh.vert[tIndex.idx[j]];
        if (polygon[j].inCore)
        {
          new_face.V(j) = &tentative_mesh.vert[polygon[j].idx];
         
        }
        else
        {
          int index = polygon[j].idx + int( mesh.inCorePoints.size() );
          new_face.V(j) = &tentative_mesh.vert[index];
        }
        
        //cout << tIndex[j].idx << ", ";
      }
      tentative_mesh.face.push_back(new_face);
      //cout << endl;
    } 

    tentative_mesh.vn = tentative_mesh.vert.size();
    tentative_mesh.fn = tentative_mesh.face.size();
    vcg::tri::UpdateNormals<CMesh>::PerVertex(tentative_mesh);


    //model->vert.clear();
    //model->face.clear();
    //for (int i = 0; i < tentative_mesh.vert.size(); i++)
    //{
    //  model->vert.push_back(tentative_mesh.vert[i]);
    //}
    //model->vn = model->vert.size();

    //for (int i = 0; i < tentative_mesh.face.size(); i++)
    //{
    //  model->face.push_back(tentative_mesh.face[i]);
    //}
    //model->fn = model->face.size();
    //vcg::tri::MyAllocator<CMesh>::CompactFaceVector(*model);  
    //vcg::tri::MyAllocator<CMesh>::CompactVertexVector(*model);

    float radius = 0;
    int sampleNum = para->getDouble("Poisson Disk Sample Number");
    if (sampleNum <= 100)
    {
      sampleNum = 100;
    }
    radius = tri::SurfaceSampling<CMesh,BaseSampler>::ComputePoissonDiskRadius(tentative_mesh, sampleNum);

    // first of all generate montecarlo samples for fast lookup
    CMesh *presampledMesh=&(tentative_mesh);
    CMesh MontecarloMesh; // this mesh is used only if we need real poisson sampling (and therefore we need to choose points different from the starting mesh vertices)

    if (1)
    {
      BaseSampler sampler(&MontecarloMesh);
      sampler.qualitySampling =true;
      tri::SurfaceSampling<CMesh,BaseSampler>::Montecarlo(tentative_mesh, sampler, sampleNum*20);
      MontecarloMesh.bbox = tentative_mesh.bbox; // we want the same bounding box
      presampledMesh=&MontecarloMesh;
    }

    iso_points->vert.clear();
    BaseSampler mps(iso_points);
    tri::SurfaceSampling<CMesh,BaseSampler>::PoissonDiskParam pp;
    tri::SurfaceSampling<CMesh,BaseSampler>::PoissonDisk(tentative_mesh, mps, *presampledMesh, radius,pp);

    for (int i = 0; i < iso_points->vert.size(); i++)
    {
      CVertex& v = iso_points->vert[i];
      v.is_iso = true;
      v.m_index = i;
      v.eigen_confidence = 0;
      v.N().Normalize();
      v.recompute_m_render();
    }
    iso_points->vn = iso_points->vert.size();


    iso_points->face.clear();
    for (int i = 0; i < tentative_mesh.face.size(); i++)
    {
      iso_points->face.push_back(tentative_mesh.face[i]);
    }
    iso_points->fn = iso_points->face.size();

 /*   vector<Point3f> sample_points;
    float result_radius;
    tri::PoissonSampling(tentative_mesh, sample_points, 2500, result_radius);

    iso_points->vert.clear();
    for (int i = 0; i < sample_points.size(); i++)
    {
      CVertex new_v;
      new_v.m_index = i; 
      new_v.is_iso = true;
      new_v.eigen_confidence = 0;
      new_v.P() = sample_points[i];
      iso_points->vert.push_back(new_v);
      iso_points->bbox.Add(new_v.P());
    }
    iso_points->vn = iso_points->vert.size();*/

  }
  else
  {
    //global_paraMgr.glarea.setValue("Show ISO Points", BoolValue(false));
    //global_paraMgr.poisson.setValue("Show Slices Mode", BoolValue(true));

    int res;
    Pointer( Real ) grid_values = tree.GetSolutionGrid( res , isoValue , Par.VoxelDepth );

    float tree_scale = tree._scale;
    Point3D<float> tree_center = tree._center;

    float space = tree_scale * (1.0 / (1<<Par.Depth));

    Point3f center_p(tree_center.coords[0], tree_center.coords[1], tree_center.coords[2]);

    int index = 0;
    field_points->vert.clear();
    int res2 = res * res;
    for (int i = 0; i < res; i++)
    {
      for (int j = 0; j < res; j++)
      {
        for (int k = 0; k < res; k++)
        {
          Point3f p(i * space, j * space, k * space);
          CVertex new_v;
          new_v.is_iso = true;
          new_v.P() = p + center_p;
          new_v.m_index = index;
          new_v.eigen_confidence = float( grid_values[i + j * res + k * res2] );          
          index++;
          field_points->vert.push_back(new_v);
          field_points->bbox.Add(new_v.P());
        }
      }
    }

    field_points->vn = field_points->vert.size();
    cout << "field point size:  " << field_points->vn << endl;
    cout << "resolution:  " << res << endl;
  }

}

void Poisson::runSlice()
{
  if (field_points->vert.empty())
  {
    return;
  }
  int iso_num = field_points->vert.size();
  double show_percentage = para->getDouble("Show Slice Percentage");
  bool paraller_slice_mode = para->getBool("Parallel Slices Mode");

  int res = 0;
  for (; res < iso_num; res++)
  {
    if (res * res * res >= iso_num)
    {
      break;
    }
  }
  int res2 = res * res;

  show_percentage = (std::max)(0., show_percentage);
  show_percentage = (std::min)(1., show_percentage);
  int begin = int(res * (1-show_percentage));
  int end = int(res * show_percentage);
  end = (std::max)(end, begin+1);

  if (para->getBool("Show X Slices"))
  {
    double slice_i_position = para->getDouble("Current X Slice Position");
    if (!paraller_slice_mode)
    {
      int slice_i_num = res * slice_i_position;

      (*slices)[0].slice_nodes.clear();
      for (int i = slice_i_num; i < slice_i_num + 1; i++)
      {
        for (int j = begin; j < end; j++)
        {
          for (int k = begin; k < end; k++)
          {      
            (*slices)[0].slice_nodes.push_back(field_points->vert[i * res2 + j * res + k]);
          }
        }
      }
      (*slices)[0].res = end - begin;
    }
    else
    {
      int slice_k_num = res * slice_i_position;

      (*slices)[0].slice_nodes.clear();
      for (int i = begin; i < end; i++)
      {
        for (int j = begin; j < end; j++)
        {
          for (int k = slice_k_num; k < slice_k_num+1; k++)
          {      
            (*slices)[0].slice_nodes.push_back(field_points->vert[i * res2 + j * res + k]);
          }
        }
      }
      (*slices)[0].res = end - begin;
    }
  }

  if (para->getBool("Show Y Slices"))
  {
    double slice_j_position = para->getDouble("Current Y Slice Position");

    if (!paraller_slice_mode)
    {
      int slice_j_num = res * slice_j_position;

      (*slices)[1].slice_nodes.clear();
      for (int i = begin; i < end; i++)
      {
        for (int j = slice_j_num; j < slice_j_num+1; j++)
        {
          for (int k = begin; k < end; k++)
          {      
            (*slices)[1].slice_nodes.push_back(field_points->vert[i * res2 + j * res + k]);
          }
        }
      }
      (*slices)[1].res = end - begin;
    }
    else
    {
      int slice_k_num = res * slice_j_position;
      (*slices)[1].slice_nodes.clear();
      for (int i = begin; i < end; i++)
      {
        for (int j = begin; j < end; j++)
        {
          for (int k = slice_k_num; k < slice_k_num+1; k++)
          {      
            (*slices)[1].slice_nodes.push_back(field_points->vert[i * res2 + j * res + k]);
          }
        }
      }
      (*slices)[1].res = end - begin;
    }

  }

  if (para->getBool("Show Z Slices"))
  {
    double slice_k_position = para->getDouble("Current Z Slice Position");
    int slice_k_num = res * slice_k_position;

    (*slices)[2].slice_nodes.clear();
    for (int i = begin; i < end; i++)
    {
      for (int j = begin; j < end; j++)
      {
        for (int k = slice_k_num; k < slice_k_num+1; k++)
        {      
          (*slices)[2].slice_nodes.push_back(field_points->vert[i * res2 + j * res + k]);
        }
      }
    }
    (*slices)[2].res = end - begin;
  }
}


void Poisson::runComputeOriginalConfidence()
{
  //vector<Point3f> result;
  //float result_radius;
  //tri::PoissonSampling(*model, result, 2000, result_radius);

  //iso_points->vert.clear();
  //for (int i = 0; i < result.size(); i++)
  //{
  //  CVertex new_v;
  //  new_v.m_index = i; 
  //  new_v.is_iso = true;
  //  new_v.P() = result[i];
  //  iso_points->vert.push_back(new_v);
  //}
  //iso_points->vn = iso_points->vert.size();



  //vector< vector<float>>confidences;
  //
  //int factors = 0;
  //if (para->getBool("Use Confidence 1")) factors++;
  //if (para->getBool("Use Confidence 2")) factors++;
  //if (para->getBool("Use Confidence 3")) factors++;
  //if (para->getBool("Use Confidence 4")) factors++;

  //if (factors == 0)
  //{
  //  return;
  //}
  //
  //vector<float> temp(factors, 0);
  //confidences.assign(original->vn, temp);
  //
  //double radius = para->getDouble("CGrid Radius");
  //GlobalFun::computeBallNeighbors(original, NULL, 
  //                                radius, 
  //                                original->bbox);
  //double radius2 = radius * radius;
  //double iradius16 = -4.0 / radius2;

  //int curr = 0;
  //if (para->getBool("Use Confidence 1"))
  //{
  //  //float sum_confidence = 0;
  //  float min_confidence = GlobalFun::getDoubleMAXIMUM();
  //  float max_confidence = 0;
  //  for (int i = 0; i < original->vert.size(); i++)
  //  {
  //    confidences[i][curr] = 1;
  //    CVertex& v = original->vert[i];
  //    vector<int>* neighbors = &v.neighbors;
  //    for (int j = 0; j < v.neighbors.size(); j++)
  //    {
  //      CVertex& t = original->vert[(*neighbors)[j]];
  //      float dist2  = (v.P() - t.P()).SquaredNorm();
  //      float den = exp(dist2*iradius16);

  //      confidences[i][curr] += den;
  //    }
  //    min_confidence = (std::min)(min_confidence, confidences[i][curr]);
  //    max_confidence = (std::max)(max_confidence, confidences[i][curr]);
  //  }

  //  float space = max_confidence - min_confidence;
  //  for (int i = 0; i < original->vert.size(); i++)
  //  {
  //    confidences[i][curr] = (confidences[i][curr] - min_confidence) / space;
  //    confidences[i][curr] -= 0.5;
  //  }
  //}

  //if (para->getBool("Use Confidence 2"))
  //{

  //}

  //if (para->getBool("Use Confidence 3"))
  //{

  //}

  //if (para->getBool("Use Confidence 4"))
  //{

  //}

  //for (int i = 0; i < original->vn; i++)
  //{
  //  CVertex& v = original->vert[i];
  //  float sum_confidences = 0;
  //  for (int j = 0; j < factors; j++)
  //  {
  //    sum_confidences += confidences[i][j];
  //  }

  //  v.eigen_confidence = sum_confidences / factors;
  //}

}

struct sort_item
{
  float value;
  int index;

  bool operator < (const sort_item& item)
  {
    return value < item.value;
  }

};

void Poisson::runComputeSampleConfidence()
{
  vector< vector<float>>confidences;

  ofstream file1("certainty_1_wlop.txt");
  ofstream file2("certainty_2_gradient.txt");
  ofstream file3("certainty_3_combine.txt");
  ofstream file4("certainty_4_combine.txt");

  int factors = 0;
  if (para->getBool("Use Confidence 1")) factors++;
  if (para->getBool("Use Confidence 2")) factors++;
  if (para->getBool("Use Confidence 3")) factors++;
  if (para->getBool("Use Confidence 4")) factors++;

  if (factors == 0)
  {
    return;
  }

  vector<float> temp(factors, 0);
  confidences.assign(samples->vn, temp);

  double radius = para->getDouble("CGrid Radius");

  double radius2 = radius * radius;
  double iradius16 = -4.0 / radius2;

  int curr = 0;
  if (para->getBool("Use Confidence 1"))
  {
    GlobalFun::computeBallNeighbors(samples, original, 
                                    radius, 
                                    original->bbox);
    //float sum_confidence = 0;
    float min_confidence = GlobalFun::getDoubleMAXIMUM();
    float max_confidence = 0;
    for (int i = 0; i < samples->vert.size(); i++)
    {
      confidences[i][curr] = 1;
      CVertex& v = samples->vert[i];
      vector<int>* neighbors = &v.original_neighbors;
      for (int j = 0; j < v.original_neighbors.size(); j++)
      {
        CVertex& t = original->vert[(*neighbors)[j]];
        float dist2  = (v.P() - t.P()).SquaredNorm();
        float den = exp(dist2*iradius16);

        confidences[i][curr] += den;
      }
      min_confidence = (std::min)(min_confidence, confidences[i][curr]);
      max_confidence = (std::max)(max_confidence, confidences[i][curr]);
    }

    float space = max_confidence - min_confidence;
    //for (int i = 0; i < samples->vert.size(); i++)
    //{
    //  confidences[i][curr] = (confidences[i][curr] - min_confidence) / space;
    //  file1 << confidences[i][curr] << endl;
    //}
    curr++;
  }

  
  if (para->getBool("Use Confidence 2"))
  {
    GlobalFun::computeBallNeighbors(samples, NULL, 
                                    radius, 
                                    original->bbox);
    double sigma = global_paraMgr.norSmooth.getDouble("Sharpe Feature Bandwidth Sigma");
    double sigma_threshold = pow(max(1e-8,1-cos(sigma/180.0*3.1415926)), 2);

    float min_confidence = GlobalFun::getDoubleMAXIMUM();
    float max_confidence = 0;
    for (int i = 0; i < samples->vert.size(); i++)
    {
      confidences[i][curr] = 0;
      CVertex& v = samples->vert[i];
      vector<int>* neighbors = &v.neighbors;
      double sum_w = 0;
      for (int j = 0; j < v.neighbors.size(); j++)
      {
        CVertex& t = samples->vert[(*neighbors)[j]];
        float dist2  = (v.P() - t.P()).SquaredNorm();
        float w = exp(dist2 * iradius16);
        double normal_diff = exp(-pow(1-v.N()*t.N(), 2)/sigma_threshold);

        confidences[i][curr] += w * normal_diff;
        sum_w += w;
      }
      confidences[i][curr] /= sum_w;

      min_confidence = (std::min)(min_confidence, confidences[i][curr]);
      max_confidence = (std::max)(max_confidence, confidences[i][curr]);
    }

    //float space = max_confidence - min_confidence;
    //for (int i = 0; i < samples->vert.size(); i++)
    //{
    //  confidences[i][curr] = (confidences[i][curr] - min_confidence) / space;
    //  file2 << confidences[i][curr] << endl;
    //}
    curr++;
  }

  if (para->getBool("Use Confidence 3"))
  {
    if (!para->getBool("Use Confidence 1"))
    {
      GlobalFun::computeBallNeighbors(samples, original, 
        radius, 
        original->bbox);
    }


    float min_confidence = GlobalFun::getDoubleMAXIMUM();
    float max_confidence = 0;
    for (int i = 0; i < samples->vert.size(); i++)
    {
      confidences[i][curr] = 0;
      CVertex& v = samples->vert[i];
      vector<int>* neighbors = &v.original_neighbors;
      double sum_w = 0;
      for (int j = 0; j < v.original_neighbors.size(); j++)
      {
        CVertex& t = original->vert[(*neighbors)[j]];
        Point3f diff = v.P() - t.P();
        float dist2  = diff.SquaredNorm();
        float w = exp(dist2 * iradius16);
        double hn = diff * v.N();
        double proj = exp(hn * hn * iradius16);

        confidences[i][curr] += w * proj;
        sum_w += w;
      }
      confidences[i][curr] /= sum_w;

      min_confidence = (std::min)(min_confidence, confidences[i][curr]);
      max_confidence = (std::max)(max_confidence, confidences[i][curr]);
    }

    //float space = max_confidence - min_confidence;
    //for (int i = 0; i < samples->vert.size(); i++)
    //{
    //  confidences[i][curr] = (confidences[i][curr] - min_confidence) / space;
    //  file3 << confidences[i][curr] << endl;
    //}
    curr++;
  }

  //if (para->getBool("Use Sort Confidence Combination"))
  if (para->getBool("Use Confidence 4"))
  {
    for (int i = 0; i < samples->vn; i++)
    {
      CVertex& v = samples->vert[i];

      float sum_confidence = 0;
      for (int j = 0; j < factors; j++)
      {
        sum_confidence += confidences[i][j] * confidences[i][j];
      }

      v.eigen_confidence = std::sqrt(sum_confidence);
      //cout << "combine confidence" << v.eigen_confidence << endl;
      //v.eigen_confidence -= 0.5;
    }

    //normalizeConfidence(samples->vert, -0.5);
    normalizeConfidence(samples->vert, 0);

    for (int i = 0; i < samples->vn; i++)
    {
      CVertex& v = samples->vert[i];
      file4 << v.eigen_confidence << endl;
    }

  }
  else
  {
    for (int i = 0; i < samples->vn; i++)
    {
      CVertex& v = samples->vert[i];
      float multiply_confidence = 1.0;
      for (int j = 0; j < factors; j++)
      {
        multiply_confidence *= confidences[i][j];
      }
      v.eigen_confidence = multiply_confidence;
    }

    normalizeConfidence(samples->vert, 0);

    for (int i = 0; i < samples->vn; i++)
    {
      CVertex& v = samples->vert[i];
      file4 << v.eigen_confidence << endl;
    }
  }
  //else
  //{
  //  float min_confidence = GlobalFun::getDoubleMAXIMUM();
  //  float max_confidence = 0;
  //  for (int i = 0; i < samples->vn; i++)
  //  {
  //    CVertex& v = samples->vert[i];
  //    float sum_confidences = 0;
  //    for (int j = 0; j < factors; j++)
  //    {
  //      sum_confidences += confidences[i][j];
  //    }

  //    v.eigen_confidence = sum_confidences / factors;

  //    min_confidence = (std::min)(min_confidence, v.eigen_confidence);
  //    max_confidence = (std::max)(max_confidence, v.eigen_confidence);
  //  }

  //  float space = max_confidence - min_confidence;
  //  for (int i = 0; i < samples->vn; i++)
  //  {
  //    CVertex& v = samples->vert[i];
  //    v.eigen_confidence = (v.eigen_confidence - min_confidence) / space;

  //    v.eigen_confidence -= 0;
  //    //cout << "combine confidence" << v.eigen_confidence << endl;
  //  }
  //}

}

void Poisson::runComputeIsoConfidence()
{
  ofstream file1("confidence_1_wlop.txt");
  ofstream file2("confidence_2_gradient.txt");
  ofstream file3("confidence_3_combine.txt");

  if (para->getBool("Use Confidence 4"))
  {
    runLabelISO();
    normalizeConfidence(iso_points->vert, 0);
  }
  vector<float> confidences_temp;
  iso_points->vn = iso_points->vert.size();
  for (int i = 0; i < iso_points->vn; i++)
  {
    file1 << iso_points->vert[i].eigen_confidence << endl;
    confidences_temp.push_back(iso_points->vert[i].eigen_confidence);
  }

  if (field_points->vert.empty())
  {
    cout << "need field points" << endl;
    return;
  }

  double radius = para->getDouble("CGrid Radius");
  double radius2 = radius * radius;
  double iradius16 = -4.0 / radius2;

  double sigma = global_paraMgr.norSmooth.getDouble("Sharpe Feature Bandwidth Sigma");
  //double sigma = 35;
  double sigma_threshold = pow(max(1e-8,1-cos(sigma/180.0*3.1415926)), 2);

  GlobalFun::computeBallNeighbors(iso_points, field_points, 
                                  radius, field_points->bbox);

  for (int i = 0; i < iso_points->vn; i++)
  {
    CVertex& v = iso_points->vert[i];

    float positive_sum = 0.0;
    float negative_sum = 0.0;
    float positive_w_sum = 0.0;
    float negative_w_sum = 0.0;

    for (int j = 0; j < v.original_neighbors.size(); j++)
    {
      int index = v.original_neighbors[j];
      CVertex& t = field_points->vert[index];

      Point3f diff = t.P() - v.P();
      Point3f vn = v.N();
      float proj = diff * v.N();

      float dist2  = diff.SquaredNorm();
      float w1 = exp(dist2 * iradius16);
      float w2 = 1.0;
      if (1)
      {
        if (proj > 0)
        {
          w2 = exp(-pow(1-vn*diff.Normalize(), 2)/sigma_threshold); 
        }
        else
        {
          vn *= -1;
          w2 = exp(-pow(1-vn*diff.Normalize(), 2)/sigma_threshold); 
        } 
      }
      float w = w1 * w2;

      if (proj > 0)
      {
        positive_sum += w * t.eigen_confidence;
        positive_w_sum += w;
      }
      else
      {
        negative_sum += w * t.eigen_confidence;
        negative_w_sum += w;
      }
    }

    if (positive_w_sum > 0 && negative_w_sum > 0)
    {
      v.eigen_confidence = abs(positive_sum / positive_w_sum - negative_sum / negative_w_sum);
      //cout << v.eigen_confidence << endl;
    }
  }
  normalizeConfidence(iso_points->vert, 0);

  
  if (para->getBool("Use Confidence 4"))
  {
    if (para->getBool("Use Confidence 3"))
    {

      for (int i = 0; i < iso_points->vn; i++)
      {
        CVertex& v = iso_points->vert[i];
        file2 << v.eigen_confidence << endl;
        float temp_confidence = confidences_temp[i];
        float combine_confidence = std::sqrt(temp_confidence * temp_confidence 
                                   + v.eigen_confidence * v.eigen_confidence);
        v.eigen_confidence = combine_confidence;
      }

      normalizeConfidence(iso_points->vert, 0);

    }
    else
    {
      for (int i = 0; i < iso_points->vn; i++)
      {
        CVertex& v = iso_points->vert[i];
        file2 << v.eigen_confidence << endl;
        float temp_confidence = confidences_temp[i];
        v.eigen_confidence *= temp_confidence;
      }

      normalizeConfidence(iso_points->vert, 0);
    }

    for (int i = 0; i < iso_points->vn; i++)
    {
      CVertex& v = iso_points->vert[i];
      file3 << v.eigen_confidence << endl;
    }
  }



}

void Poisson::normalizeConfidence(vector<CVertex>& vertexes, float delta)
{
  float min_confidence = GlobalFun::getDoubleMAXIMUM();
  float max_confidence = 0;
  for (int i = 0; i < vertexes.size(); i++)
  {
    CVertex& v = vertexes[i];
    min_confidence = (std::min)(min_confidence, v.eigen_confidence);
    max_confidence = (std::max)(max_confidence, v.eigen_confidence);
  }
  float space = max_confidence - min_confidence;

  for (int i = 0; i < vertexes.size(); i++)
  {
    CVertex& v = vertexes[i];
    v.eigen_confidence = (v.eigen_confidence - min_confidence) / space;

    v.eigen_confidence += delta;
  }

}
