#pragma once
#include <iostream>
#include <tbb/parallel_for.h>
#include "PointCloudAlgorithm.h"
#include "GlobalFunction.h"
#include "NBVGrid.h"

using std::cout;
using std::endl;
using vcg::Point3f;

#define  LINKED_WITH_TBB

class NBV : public PointCloudAlgorithm
{
public:
  NBV(RichParameterSet* _para);
  ~NBV();

  void run();
  void setInput(DataMgr *pData);
  void setParameterSet(RichParameterSet *_para) { para = _para;}
  RichParameterSet * getParameterSet() { return para;}
  void clear();

private:
  void buildGrid();
  void propagate();

  void runOneKeyNBV();

  void viewExtraction();
  void viewExtractionIntoBins();
  void extractViewIntoBinsUsingDist();
  void viewClustering();
  void setIsoBottomConfidence();
  bool updateViewDirections();
  
  void normalizeConfidence(vector<CVertex>& vertexes, float delta);
  double   getAbsMax(double x, double y, double z);
  int      round(double x);
  quadrant getQuadrantIdx(double a, double b); //two parameters deciding the quadrant
  void     setGridUnHit(vector<int>& hit_grids_idx);
  double computeLocalScores(CVertex& view_t, CVertex& iso_v, 
                            double& optimal_D, double& half_D2, double& sigma_threshold);
  int getIsoPointsViewBinIndex(Point3f& p, int which_axis);

private:
  RichParameterSet      *para;
  //CMesh                 *model;
  CMesh                 *original;
  CMesh                 *iso_points;
  CMesh                 *view_grid_points;
  CMesh                 *nbv_candidates;
  vector<ScanCandidate> *scan_candidates;
  CMesh                 *field_points;
  double                grid_step_size;
  Point3f               whole_space_box_max;
  Point3f               whole_space_box_min;
  int                   x_max; //max index num of x-axis 
  int                   y_max;
  int                   z_max;
  std::vector<NBVGrid>  *view_grids; //grids in all the space
  vector<float>         confidence_weight_sum;
  static int            view_bins_each_axis;
  vector<double>        nbv_scores;
  Box3f* whole_space_box;
};