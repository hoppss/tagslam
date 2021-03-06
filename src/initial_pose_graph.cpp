/* -*-c++-*--------------------------------------------------------------------
 * 2018 Bernd Pfrommer bernd.pfrommer@gmail.com
 */

#include "tagslam/initial_pose_graph.h"
#include "tagslam/utils.h"
#include "tagslam/resectioning_factor.h"
#include <boost/range/irange.hpp>
#include <gtsam/slam/expressions.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/ExpressionFactorGraph.h>
#include <boost/random/normal_distribution.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <math.h> // isnormal

namespace tagslam {
  using namespace boost::random;
  using boost::irange;
  typedef boost::random::mt19937 RandEng;
  typedef boost::random::normal_distribution<double> RandDist;
  typedef boost::random::variate_generator<RandEng, RandDist> RandGen;

  static gtsam::Pose3 make_random_pose(RandGen *rgr, RandGen *rgt) {
    gtsam::Point3 t((*rgt)(), (*rgt)(), (*rgt)());
    gtsam::Point3 om((*rgr)(), (*rgr)(), (*rgr)());
    return (gtsam::Pose3(gtsam::Rot3::rodriguez(om.x(),om.y(),om.z()), gtsam::Point3(t)));
  }
#if 0
  static void print_pose(const gtsam::Pose3 &p) {
    const auto mat = p.matrix();
    std::cout << "[" << mat(0,0) << ", " << mat(0,1) << ", " << mat(0,2) << ", " << mat(0,3) << "; " << std::endl;
    std::cout << " " << mat(1,0) << ", " << mat(1,1) << ", " << mat(1,2) << ", " << mat(1,3) << "; " << std::endl;
    std::cout << " " << mat(2,0) << ", " << mat(2,1) << ", " << mat(2,2) << ", " << mat(2,3) << "; " << std::endl;
    std::cout << " " << mat(3,0) << ", " << mat(3,1) << ", " << mat(3,2) << ", " << mat(3,3) << "; " << std::endl;
    std::cout << "];" << std::endl;
  }
#endif


  static double get_graph_error(const gtsam::Pose3  &pose,
                                const gtsam::Values &values,
                                const gtsam::NonlinearFactorGraph &graph) {
    gtsam::Values v = values;
    gtsam::Symbol P = gtsam::Symbol('P', 0); // pose symbol
    v.insert(P, pose);
    double e = graph.error(v);
    if (!std::isnormal(e)) {
      throw (std::runtime_error("bad starting guess for graph error"));
    }
    return (e);
  }

  static PoseEstimate try_optimization(const gtsam::Pose3  &startPose,
                                       const gtsam::Values &startValues,
                                       gtsam::NonlinearFactorGraph *graph) {
    gtsam::Symbol P = gtsam::Symbol('P', 0); // pose symbol
    gtsam::Values                 values = startValues;
    gtsam::Values                 optimizedValues;
    const int MAX_ITER = 100;
    values.insert(P, startPose);
    try {
      // if the starting pose is bad (e.g. the camera is in the
      // plane of the world points), then the optimizer will
      // encounter NaNs and get stuck in an infinite loop.
      // That's why we test here first if the error is reasonable.
      if (!std::isnormal(graph->error(values))) {
        throw (std::runtime_error("bad starting guess"));
      }
      gtsam::LevenbergMarquardtParams lmp;
      lmp.setVerbosity("SILENT");
      lmp.setMaxIterations(MAX_ITER);
      lmp.setAbsoluteErrorTol(1e-7);
      lmp.setRelativeErrorTol(0);
      gtsam::LevenbergMarquardtOptimizer lmo(*graph, values, lmp);

      optimizedValues = lmo.optimize();
      gtsam::Pose3 op = optimizedValues.at<gtsam::Pose3>(P);
      return (PoseEstimate(op, (double)lmo.error() / graph->size(),
                           (int)lmo.iterations()));
    } catch (const std::exception &e) {
      // bombed out because of cheirality etc
    }
    return (PoseEstimate(startPose, 1e10, MAX_ITER));
  }

#if 0  
  static void analyze_pose(const CameraVec &cams,
                           const ImageVec &imgs,
                           bool debug,
                           unsigned int frameNum,
                           const RigidBodyConstPtr &rb,
                           const gtsam::Pose3 &bodyPose) {
    const cv::Scalar origColor(0,255,0), projColor(255,0,255);
    const cv::Size rsz(4,4);

    for (const auto &tagMap: rb->observedTags) {
      int cam_idx = tagMap.first;
      std::cout << "points + projected for cam " << cam_idx << std::endl;
      const CameraPtr &cam = cams[cam_idx];
      std::cout << "cam pose: " << std::endl;
      print_pose(cam->poseEstimate.getPose());
      if (!cam->poseEstimate.isValid()) {
        continue;
      }
      
      gtsam::PinholeCamera<Cal3FS2> phc(cam->poseEstimate.getPose(), *cam->equidistantModel);
      std::vector<gtsam::Point3> bpts;
      std::vector<gtsam::Point3> wpts;
      std::vector<gtsam::Point2> ipts;
      rb->getAttachedPoints(cam_idx, &bpts, &ipts, false);
      for (const auto i: irange(0ul, bpts.size())) {
        wpts.push_back(bodyPose.transform_from(bpts[i]));
      }
      std::cout << "ppts=[ ";
      cv::Mat img;
      if (cam_idx < (int)imgs.size()) img = imgs[cam_idx];
      for (const auto i: irange(0ul, wpts.size())) {
        gtsam::Point3 wp  = wpts[i];
        gtsam::Point2 icp = phc.project(wp);
        gtsam::Point2 d = icp - ipts[i];

        std::cout << wpts[i].x() << "," << wpts[i].y() << "," << wpts[i].z() << ", " << ipts[i].x() << ", " << ipts[i].y() << ", " << icp.x() << ", " << icp.y() << ", " << d.x() << ", " << d.y() << ";" <<  std::endl;
        if (debug && img.rows > 0) {
          cv::rectangle(img, cv::Rect(cv::Point2d(ipts[i].x(), ipts[i].y()), rsz), origColor, 2, 8, 0);
          cv::rectangle(img, cv::Rect(cv::Point2d(icp.x(), icp.y()), rsz), projColor, 2, 8, 0);
        }
      }
      std::cout << "];" << std::endl;
      if (debug && img.rows > 0) {
        std::string fbase = "image_" + std::to_string(frameNum) + "_";
        cv::imwrite(fbase + std::to_string(cam_idx) + ".jpg", img);
      }
    }
    
  }
#endif  

//#define DEBUG_BODY_POSE
  PoseEstimate
  InitialPoseGraph::estimateBodyPose(const CameraVec &cams,
                                     const ImageVec &imgs,
                                     unsigned int frameNum,
                                     const RigidBodyConstPtr &rb,
                                     const gtsam::Pose3 &initialPose,
                                     double *errorLimit) const {
    //std::cout << "----------------- analysis of initial pose -----" << std::endl;
    //analyze_pose(cams, imgs, false, frameNum, rb, initialPose);
    PoseEstimate pe; // defaults to invalid
    gtsam::ExpressionFactorGraph  graph;
#ifdef DEBUG_BODY_POSE
    std::cout << "estimating body pose from cameras: " << rb->observedTags.size() << std::endl;
    std::cout << "initial guess pose: " << std::endl;
    print_pose(initialPose);
#endif    
    // loop through all tags on body
    auto pixelNoise = gtsam::noiseModel::Isotropic::Sigma(2, 1.0);
    gtsam::Pose3_  T_w_b('P', 0);
    std::vector<gtsam::Point2> all_ip;
    for (const auto &tagMap: rb->observedTags) {
      int cam_idx = tagMap.first;
      const CameraPtr &cam = cams[cam_idx];
      if (!cam->poseEstimate.isValid()) {
        continue;
      }
      gtsam::Pose3_ T_r_c  = cam->poseEstimate.getPose();
      gtsam::Pose3_ T_w_r  = cam->rig->poseEstimate.getPose();
#ifdef DEBUG_BODY_POSE
      std::cout << "camera " << cam_idx << " pose: " << std::endl;
      print_pose(cam->poseEstimate.getPose());
#endif      
      std::vector<gtsam::Point3> bp;
      std::vector<gtsam::Point2> ip;
      std::vector<gtsam::Pose3>  T_w_o;

      rb->getAttachedPoints(cam_idx, &bp, &ip, &T_w_o,
                            false /* get world points in body frame! */);
      // now add points to graph

#ifdef DEBUG_BODY_POSE
      std::cout << "cam points: " << std::endl;
      std::cout << "pts=[" << std::endl;
#endif      
      for (const auto i: irange(0ul, bp.size())) {
        gtsam::Point3_ p(bp[i]);
        all_ip.push_back(ip[i]);
#ifdef DEBUG_BODY_POSE
        std::cout << bp[i].x() << "," << bp[i].y() << "," << bp[i].z() << "," << ip[i].x() << ", " << ip[i].y() << ";" << std::endl;
#endif        
        //std::cout << "transform to camera: T_c_w= " << std::endl << cam->poseEstimate.getPose().inverse() << std::endl;
        //std::cout << "transformed point: p: " << std::endl << cam->poseEstimate.getPose().inverse().transform_to(bp[i]) << std::endl;
        // P_A = transform_from(T_AB, P_B)
        //
        // X_c = T_c_r * T_r_w * T_w_b * X_b
        gtsam::Point2_ xp = gtsam::project(gtsam::transform_to(T_r_c, gtsam::transform_to(T_w_r, gtsam::transform_from(T_w_b, p))));
        if (cam->radtanModel) {
          gtsam::Expression<Cal3DS3> cK(*cam->radtanModel);
          gtsam::Point2_ predict(cK, &Cal3DS3::uncalibrate, xp);
          graph.addExpressionFactor(predict, ip[i], pixelNoise);
        } else if (cam->equidistantModel) {
          gtsam::Expression<Cal3FS2> cK(*cam->equidistantModel);
          gtsam::Point2_ predict(cK, &Cal3FS2::uncalibrate, xp);
          graph.addExpressionFactor(predict, ip[i], pixelNoise);
        }
      }
#ifdef DEBUG_BODY_POSE
      std::cout << "];" << std::endl;
#endif      
    }
    gtsam::Values initialValues;
    double pixelError = initRelPixErr_ * utils::get_pixel_range(all_ip);
    pe = optimizeGraph(initialPose, initialValues, &graph, pixelError,
                       errorLimit, std::vector<gtsam::Point3>());
#ifdef DEBUG_BODY_POSE    
    std::cout << "optimized graph pose T_w_b: " << std::endl;
    print_pose(pe.getPose());
    std::cout << "pose graph error: " << pe.getError() << std::endl;
#endif    
    //std::cout << "----------------- analysis of final pose -----" << std::endl;
    //analyze_pose(cams, imgs, false, frameNum, rb, pe.getPose());
    return (pe);
  }

  
  // returns T_w_c
  PoseEstimate
  InitialPoseGraph::estimateCameraPose(const CameraPtr &camera,
                                       const std::vector<gtsam::Point3> &wp,
                                       const std::vector<gtsam::Point2> &ip,
                                       const PoseEstimate &initialPose,
                                       double *errorLimit) const {
    PoseEstimate pe; // defaults to invalid
    if (wp.empty()) {
      return (pe);
    }

    gtsam::ExpressionFactorGraph   graph;
    auto pixelNoise = gtsam::noiseModel::Isotropic::Sigma(2, 1.0);
    gtsam::Symbol T_w_c = gtsam::Symbol('P', 0); // camera pose symbol
    for (const auto i: boost::irange(0ul, wp.size())) {
      gtsam::Point2_ xp = gtsam::project(gtsam::transform_to(T_w_c, wp[i]));
      if (camera->radtanModel) {
        gtsam::Expression<Cal3DS3> cK(*camera->radtanModel);
        gtsam::Expression<gtsam::Point2> predict(cK, &Cal3DS3::uncalibrate, xp);
        graph.addExpressionFactor(predict, ip[i], pixelNoise);
      } else if (camera->equidistantModel) {
        gtsam::Expression<Cal3FS2> cK(*camera->equidistantModel);
        gtsam::Expression<gtsam::Point2> predict(cK, &Cal3FS2::uncalibrate, xp);
        graph.addExpressionFactor(predict, ip[i], pixelNoise);
      }
    }
    gtsam::Values initialValues;
    double pixelError = initRelPixErr_ * utils::get_pixel_range(ip);
    pe = optimizeGraph(initialPose, initialValues, &graph, pixelError,
                       errorLimit, wp);
    return (pe);
  }



  PoseEstimate
  InitialPoseGraph::optimizeGraph(const gtsam::Pose3 &startPose,
                                  const gtsam::Values &startValues,
                                  gtsam::NonlinearFactorGraph *graph,
                                  double errorLimit, double *adjErrorLimit,
                                  const std::vector<gtsam::Point3> &wp) const {
  	RandEng	randomEngine;
    RandDist distTrans(0, 10.0); // mu, sigma for translation
    RandDist distRot(0, M_PI);	 // mu, sigma for rotations
    RandGen	 rgt(randomEngine, distTrans);	 // random translation generator
    RandGen  rgr(randomEngine, distRot);	   // random angle generator
    gtsam::Pose3 pose = startPose;
    PoseEstimate bestPose(startPose,
                          get_graph_error(startPose, startValues, *graph));
    int num_iter(0);
    const int MAX_NUM_ITER = 2000;
    const double MAX_ADJUST_RATIO = 5.0; // max ratio to which err limit can grow
    const double ffac = std::pow(MAX_ADJUST_RATIO, 1.0/MAX_NUM_ITER);
    double adjFac = 1.0;
    for (num_iter = 0; num_iter < MAX_NUM_ITER; num_iter++) {
      PoseEstimate pe = try_optimization(pose, startValues, graph);
      double adjustedLimit = errorLimit * adjFac;
      if (pe.getError() < bestPose.getError()) {
        if (!utils::has_negative_z(pe.getPose().inverse(), wp)) {
          bestPose = pe;
          //std::cout << num_iter << " best pose: " << pe.getError() << " vs lim: " << adjustedLimit << std::endl;
        }
      }
      if (bestPose.getError() < adjustedLimit) {
        break;
      }
      pose = make_random_pose(&rgr, &rgt);
      adjFac = adjFac * ffac; // exponentially increasing limit
    }
    if (num_iter * 10 > MAX_NUM_ITER) {
      ROS_WARN_STREAM("init pose guess took " << num_iter << " iterations, slowing you down!");
      ROS_WARN_STREAM("consider increasing initial_maximum_relative_pixel_error from " << initRelPixErr_);
    }
    if (bestPose.getError() >= errorLimit * adjFac) {
      ROS_WARN_STREAM("initialization graph failed with error " <<
                      bestPose.getError() << " vs limit: " << errorLimit * adjFac);
    }
    *adjErrorLimit = errorLimit * adjFac;
    return (bestPose);
  }
  

}  // namespace
