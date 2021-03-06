//
// OctoMap Map 3D v1
//
// Stores a PointCloud in an OctoMap (an OcTree).
// Planning using RRT variants from OMPL (Open Motion Planning Libray)
// in a RealVectorStateSpace (x,y,z position).
//
// FCL (Flexible Collision Library) is used for collision-checking.
// The robot is represented as an FCL Sphere BVH (Bounding Volume Hierarchy).
// AABB Trees are created from both the Sphere and the OcTree.
//
// During planning, a state is considered "valid" if all points within
// the robot's radius are free. The planner interpolates between
// states and makes sure the space in-between is also free.
// 
// David Butterworth
//

// This works with FCL 0.3.3 from ROS Indigo.
// Since then, the header files and interface have changed a lot.

#include <planning_through_pointclouds/octomap_map_3d_v1_ompl_fcl.h>

#include <iostream> // cout
#include <queue> // constant-time lookup of largest element
#include <algorithm> // std::reverse

#include <pcl/common/distances.h> // euclideanDistance()

#include <planning_through_pointclouds/utils.h> // range, printStdVector
#include <planning_through_pointclouds/pcl_utils.h> // loadPointCloud, downsamplePointCloud

#include <ompl/geometric/planners/rrt/RRT.h>
#include <ompl/geometric/planners/rrt/RRTConnect.h>
#include <ompl/geometric/planners/rrt/RRTstar.h>
//#include <ompl/geometric/planners/prm/PRM.h>

#include "ompl/base/objectives/PathLengthOptimizationObjective.h"

// BIT* is not available in OMPL distributed with Ubuntu, you must compile from trunk
#ifdef USE_BITSTAR
#include <ompl/geometric/planners/bitstar/BITstar.h>
#endif

#include <fcl/BVH/BVH_model.h>
#include <fcl/octree.h>
#include <fcl/shape/geometric_shapes.h> // Sphere
#include <fcl/shape/geometric_shape_to_BVH_model.h>

OctomapMap3D::OctomapMap3D(PointCloud::Ptr& input_pointcloud, const double voxel_size, const double robot_radius)
  : octree_voxel_size_(voxel_size)
  , robot_radius_(robot_radius)
{
  pointcloud_dims_ = Bounds(getPointCloudBounds<Point>(input_pointcloud));

  std::cout << "Cloud dimensions: " << std::endl
            << "  x_min: " << pointcloud_dims_.min_x << " "
            << "  x_max: " << pointcloud_dims_.max_x << std::endl
            << "  y_min: " << pointcloud_dims_.min_y << " "
            << "  y_max: " << pointcloud_dims_.max_y << std::endl
            << "  z_min: " << pointcloud_dims_.min_z << " "
            << "  z_max: " << pointcloud_dims_.max_z << std::endl;
  // ToDo: Verify max is greater than min

  std::cout << "Initializing OctoMap OcTree from PointCloud... " << std::endl;
  octomap_octree_ = boost::shared_ptr<octomap::OcTree>(new octomap::OcTree(octree_voxel_size_));

  for(PointCloud::iterator it = input_pointcloud->begin(); it != input_pointcloud->end(); ++it)
  {
    const octomap::point3d endpoint(static_cast<double>(it->x), 
                                    static_cast<double>(it->y),
                                    static_cast<double>(it->z));
    octomap_octree_->updateNode(endpoint, true, true); // is_occupied, lazy_eval
  }

  std::cout << "Creating AABB Tree for OcTree... " << std::endl;

  boost::shared_ptr<fcl::Sphere> fcl_sphere = boost::shared_ptr<fcl::Sphere>(new fcl::Sphere(robot_radius_));

  boost::shared_ptr<fcl::BVHModel<fcl::OBBRSS> > sphere_bvh_model = boost::shared_ptr<fcl::BVHModel<fcl::OBBRSS> >(new fcl::BVHModel<fcl::OBBRSS>());
  fcl::generateBVHModel(*sphere_bvh_model, *fcl_sphere, fcl::Transform3f(), 16, 16); // 16 segments, 16 rings

  fcl::Transform3f sphere_transform;
  sphere_transform.setIdentity();

  // Use BVH
  sphere_collision_object_ = new fcl::CollisionObject(boost::shared_ptr<fcl::CollisionGeometry>(sphere_bvh_model), sphere_transform);
  // Use sphere
  //sphere_collision_object_ = new fcl::CollisionObject(fcl_sphere); // this takes boost::shared_ptr

  sphere_collision_manager_ = new fcl::DynamicAABBTreeCollisionManager();
  sphere_collision_manager_->registerObject(sphere_collision_object_);
  sphere_collision_manager_->setup();

  boost::shared_ptr<fcl::OcTree> fcl_octree = boost::shared_ptr<fcl::OcTree>(new fcl::OcTree(boost::shared_ptr<const octomap::OcTree>(octomap_octree_)));
  boost::shared_ptr<fcl::CollisionGeometry> fcl_octree_geometry(fcl_octree);
  octree_collision_object_ = new fcl::CollisionObject(fcl_octree_geometry); // this takes boost::shared_ptr

  octree_collision_manager_ = new fcl::DynamicAABBTreeCollisionManager();
  octree_collision_manager_->registerObject(octree_collision_object_);
  octree_collision_manager_->setup();

  std::cout << "Done." << std::endl;
}

const std::vector<Point> OctomapMap3D::findPath(const Point& start_point,
                                               const Point& goal_point,
                                               const int planner_type,
                                               const double max_planning_time,
                                               const bool interpolate_path,
                                               const double interpolation_step)
{
  std::cout << "  Start point: " << start_point.x << "," << start_point.y << "," << start_point.z << std::endl;
  std::cout << "  Goal point: " << goal_point.x << "," << goal_point.y << "," << goal_point.z << std::endl;

  // Check if start and goal are within the PointCloud and not in collision
  checkStartAndGoalValidity(start_point, goal_point);

  // Initialize Vector state space (x,y,z linear motion)
  const int num_dimensions = 3;
  state_space_.reset(new ompl::base::RealVectorStateSpace(num_dimensions));
  ompl::base::RealVectorBounds bounds(num_dimensions);
  bounds.setLow(0,  pointcloud_dims_.min_x);
  bounds.setHigh(0, pointcloud_dims_.max_x);
  bounds.setLow(1,  pointcloud_dims_.min_y);
  bounds.setHigh(1, pointcloud_dims_.max_y);
  bounds.setLow(2,  pointcloud_dims_.min_z);
  bounds.setHigh(2, pointcloud_dims_.max_z);
  state_space_->as<ompl::base::RealVectorStateSpace>()->setBounds(bounds);
  state_space_->setup();

  space_information_ = ompl::base::SpaceInformationPtr(new ompl::base::SpaceInformation(state_space_));

  // Collision-check a sphere of the robot's radius
  space_information_->setStateValidityChecker(boost::bind(&OctomapMap3D::isStateValid_CheckSphere, this, _1));
  
  // Set collision-checking resolution (one or half voxel size).
  // In OMPL, collision-checking defaults to 0.01 (1%) of the state's maximum extent.
  const double max_extent = std::max({pointcloud_dims_.x_range, pointcloud_dims_.y_range, pointcloud_dims_.z_range}); // C++11
  const double resolution_meters = octree_voxel_size_; // / 2.0;
  const double resolution_percent = 1.0 / (max_extent / resolution_meters);
  space_information_->setStateValidityCheckingResolution(resolution_percent); 
  std::cout << "OMPL collision-checking resolution: "
            << resolution_percent << "% = " << resolution_percent*max_extent << "m"
            << "  (default: 1% = " << 0.01*max_extent << "m)" << std::endl;

  space_information_->setup();

  ompl::base::ScopedState<> start(state_space_);
  start[0] = start_point.x;
  start[1] = start_point.y;
  start[2] = start_point.z;
  //start.print(cout);
  ompl::base::ScopedState<> goal(state_space_);
  goal[0] = goal_point.x;
  goal[1] = goal_point.y;
  goal[2] = goal_point.z;
  //goal.print(cout);

  problem_definition_ = ompl::base::ProblemDefinitionPtr(new ompl::base::ProblemDefinition(space_information_));
  problem_definition_->setStartAndGoalStates(start, goal);

  double planning_time_limit = max_planning_time;

  if (planner_type == RRT)
  {
    ompl_planner_ = ompl::base::PlannerPtr(new ompl::geometric::RRT(space_information_));
  }
  else if (planner_type == RRT_CONNECT)
  {
    ompl_planner_ = ompl::base::PlannerPtr(new ompl::geometric::RRTConnect(space_information_));
  }
  else if (planner_type == RRT_STAR)
  {
    // RRT* will keep planning for max_planning_time
    ompl_planner_ = ompl::base::PlannerPtr(new ompl::geometric::RRTstar(space_information_));
  }
  else if (planner_type == BIT_STAR)
  {
    // Check that we have OMPL version with BIT*
#ifdef USE_BITSTAR
    if (max_planning_time < 0.0)
    {
      // Force the planner to stop after it finds any solution
      problem_definition_->setOptimizationObjective(this->getPathLengthObjectiveWithCostThreshold(space_information_, std::numeric_limits<double>::max()));
      // Force the solver to run by setting max planning time to 10 minutes.
      // Note: if set to the numeric limit, the planner will exit immediately!
      //planning_time_limit = std::numeric_limits<double>::max();
      planning_time_limit = 60.0*10;
    }
    // BIT* will keep planning for max_planning_time, unless set to negative value
    boost::shared_ptr<ompl::geometric::BITstar> bitstar(new ompl::geometric::BITstar(space_information_));
    std::cout << "BIT* default re-wire factor: " << bitstar->getRewireFactor() << std::endl; // 1.1
    std::cout << "BIT* default samples per batch: " << bitstar->getSamplesPerBatch() << std::endl; // 100
    std::cout << "BIT* default pruning: " << bitstar->getPruning() << std::endl; // True
    //// Set the rewiring scale factor, s, such that r_rrg = s r_rrg*.
    //bitstar->setRewireFactor(1.2);
    //bitstar->setSamplesPerBatch(100);
    //// Enable pruning of vertices/samples that CANNOT improve the current solution
    //bitstar->setPruning(true);
    ompl_planner_ = bitstar;
#else
    std::cout << "ERROR: BIT* is not available" << std::endl;
    return std::vector<Point>();
#endif
  }
  else
  {
    throw std::runtime_error("Unknown planner_type in findPath()");
  }

  ompl_planner_->setProblemDefinition(problem_definition_);
  ompl_planner_->setup();

  num_collision_checks_ = 0; // reset counter

  std::cout << "OMPL planning time limit = " << planning_time_limit << std::endl;

  ompl::base::PlannerStatus solved = ompl_planner_->solve(planning_time_limit);
  if (!solved)
  {
    std::cout << "ERROR: Failed to find path!" << std::endl;
    return std::vector<Point>();
  }

  path_length_ = problem_definition_->getSolutionPath()->length();
  boost::shared_ptr<ompl::geometric::PathGeometric> ompl_path = boost::static_pointer_cast<ompl::geometric::PathGeometric>(problem_definition_->getSolutionPath());

  if (interpolate_path)
  {
    if (interpolation_step > 0.00001)
    {
      const int num_waypoints = std::ceil(path_length_ / interpolation_step);
      // OMPL will evenly distributed the waypoints along the path length
      ompl_path->interpolate(num_waypoints);
    }
    else
    {
      // One waypoint per valid state.
      // The distance between waypoints thus depends on
      // OMPL's collision-checking resolution.
      ompl_path->interpolate();
    }
  }

  return getPathPoints(ompl_path);
}

ompl::base::OptimizationObjectivePtr OctomapMap3D::getPathLengthObjectiveWithCostThreshold(const ompl::base::SpaceInformationPtr& space_information,
                                                                                         const double cost_threshold)
{
  ompl::base::OptimizationObjectivePtr objective(new ompl::base::PathLengthOptimizationObjective(space_information));
  objective->setCostThreshold(ompl::base::Cost(cost_threshold));
  return objective;
}

// ToDo: Should check if state isvalid.
//       Also, OMPL expects this function to be thread save but isInCollision() is not.
const bool OctomapMap3D::isStateValid_CheckSphere(const ompl::base::State* state)
{
  num_collision_checks_++;

  const ompl::base::RealVectorStateSpace::StateType* state_3d = state->as<ompl::base::RealVectorStateSpace::StateType>();
  const Point center_point = Point(static_cast<float>(state_3d->values[0]),
                                   static_cast<float>(state_3d->values[1]),
                                   static_cast<float>(state_3d->values[2]));
  if (isInCollisionSphere(center_point))
  {
    return false;
  }

  return true;
}

const bool OctomapMap3D::isInCollisionSphere(const Point& center_point)
{
  CollisionData collision_data; // requests max 1 point in collision
  sphere_collision_object_->setTranslation(fcl::Vec3f(center_point.x, center_point.y, center_point.z));

  // Use sphere to octree
  //fcl::collide(sphere_collision_object_, octree_collision_object_, collision_data.request, collision_data.result);

  // Use AABB Tree's
  sphere_collision_manager_->collide(octree_collision_manager_, &collision_data, callbackCollisionFunction);

  if (collision_data.result.isCollision())
  {
    return true;
  }
  
  return false;
}

const ompl::base::PlannerDataPtr OctomapMap3D::getPlannerData()
{
  const ompl::base::PlannerDataPtr planner_data(new ompl::base::PlannerData(space_information_));
  ompl_planner_->getPlannerData(*planner_data);
  return planner_data;
}

const std::vector<Point> OctomapMap3D::getPathPoints(const boost::shared_ptr<ompl::geometric::PathGeometric> path_geometric_ptr)
{
  if (path_geometric_ptr->getStateCount() <= 0)
  {
    throw std::runtime_error("OMPL path has zero states in getPathPoints()");
  }

  // Convert path coordinates
  std::vector<Point> path_points;
  for (std::size_t i = 0; i < path_geometric_ptr->getStateCount(); ++i)
  {
    if (!path_geometric_ptr->getState(i))
    {
      throw std::runtime_error("getState() returned null ptr in getPathPoints()");
    }

    const ompl::base::RealVectorStateSpace::StateType* real_state = 
        static_cast<const ompl::base::RealVectorStateSpace::StateType*>(path_geometric_ptr->getState(i));
    const float x = real_state->values[0];
    const float y = real_state->values[1];
    const float z = real_state->values[2];

    path_points.push_back(Point(x, y, z));
  }

  return path_points;
}

const std::vector<Point> OctomapMap3D::getSimplifiedPath(const double max_time)
{
  boost::shared_ptr<ompl::geometric::PathGeometric> ompl_path = boost::static_pointer_cast<ompl::geometric::PathGeometric>(problem_definition_->getSolutionPath());
  if (!ompl_path)
  {
    throw std::runtime_error("Path is empty in getSimplifiedPath()");
  }

  ompl::geometric::PathSimplifier simplifier(space_information_);
  simplifier.simplify(*ompl_path, max_time);

  // short-cut
  //path->interpolate(100);
  //simplifier.reduceVertices(*ompl_path);

  return getPathPoints(ompl_path);
}

void OctomapMap3D::checkStartAndGoalValidity(const Point& start_point, const Point& goal_point)
{
  if ((start_point.x < pointcloud_dims_.min_x) || (start_point.x > pointcloud_dims_.max_x)
       && (start_point.y < pointcloud_dims_.min_y) || (start_point.y > pointcloud_dims_.max_y)
       && (start_point.z < pointcloud_dims_.min_z) || (start_point.z > pointcloud_dims_.max_z))
  {
    throw std::runtime_error("start point is outside extents of input PointCloud");
  }

  if ((goal_point.x < pointcloud_dims_.min_x) || (goal_point.x > pointcloud_dims_.max_x)
       && (goal_point.y < pointcloud_dims_.min_y) || (goal_point.y > pointcloud_dims_.max_y)
       && (goal_point.z < pointcloud_dims_.min_z) || (goal_point.z > pointcloud_dims_.max_z))
  {
    throw std::runtime_error("goal point is outside extents of input PointCloud");
  }

  if (isInCollisionSphere(start_point))
  {
    throw std::runtime_error("start point is in collision!");
  }

  if (isInCollisionSphere(start_point))
  {
    throw std::runtime_error("goal point is in collision!");
  }
}
