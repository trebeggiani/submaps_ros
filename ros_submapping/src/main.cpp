

#include <thread>
#include <chrono>
#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <vector>
#include <memory>
#include <stdlib.h>
#include <boost/filesystem.hpp>
#include <opencv2/opencv.hpp>

// ros
#include <ros/ros.h> 
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/Image.h>
#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

// okvis & supereightinterface
#include <okvis/DatasetReader.hpp>
#include <okvis/ThreadedSlam.hpp>
#include <okvis/TrajectoryOutput.hpp>
#include <okvis/ViParametersReader.hpp>
#include <SupereightInterface.hpp>

// visualizer
#include "Publisher.hpp"

// planner 
#include "Planner.hpp"

using namespace sensor_msgs;
using namespace message_filters;

class RosInterfacer
{
private:

  // ============= ARGS =============

  char* config_okvis;
  char* config_s8;
  char* package_dir;
  char* imu_topic;
  char* cam0_topic;
  char* cam1_topic;
  char* depth_topic;

  // ============= OKVIS + SE =============

  // okvis interface
  std::shared_ptr<okvis::ThreadedSlam> okvis_estimator;

  // supereight interface
  std::shared_ptr<SupereightInterface> se_interface;

  // to write traj estimate
  // std::shared_ptr<okvis::TrajectoryOutput> writer;

  // to configure okvis
  okvis::ViParameters parameters;

  // ============= ROS =============

  ros::NodeHandle nh;

  ros::Subscriber navgoal_sub;

  // to visualize topics in rviz
  Publisher publisher;

  ros::Subscriber imu_sub;

  image_transport::ImageTransport it; // for depth disparity callback
  image_transport::Subscriber depth_sub; // depth disparity subscriber

  message_filters::Subscriber<Image> image0_sub;
  message_filters::Subscriber<Image> image1_sub;
  typedef sync_policies::ApproximateTime<Image, Image> MySyncPolicy;
  message_filters::Synchronizer<MySyncPolicy> sync;

  // ============= PLANNER =============

  std::shared_ptr<Planner> planner;

  // ============= SOME PVT FUNCTIONS =============

  void slam_loop();

  // ============= THREADS =============

  std::thread thread_okvis;

  std::thread thread_planner;
  
public:

  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  RosInterfacer() = delete;

  RosInterfacer(char** argv);

  ~RosInterfacer();

  // Starts the processing loop
  int start();

  // ROS callbacks
  void navGoalCallback(const geometry_msgs::Point &msg);
  void imuCallback(const sensor_msgs::ImuConstPtr& msg);
  void imgsCallback(const sensor_msgs::ImageConstPtr& img_0, const sensor_msgs::ImageConstPtr& img_1);
  void depthCallback(const sensor_msgs::ImageConstPtr& img);

};

RosInterfacer::RosInterfacer(char** argv) : nh("ros_interface") , publisher(nh), it(nh),
image0_sub(nh, std::string(argv[5]), 1000), image1_sub(nh, std::string(argv[6]), 1000),
sync(MySyncPolicy(1000), image0_sub, image1_sub)
{

  // Sets the args
  config_okvis = argv[1];
  config_s8 = argv[2];
  package_dir = argv[3];
  imu_topic = argv[4];
  cam0_topic = argv[5];
  cam1_topic = argv[6];
  depth_topic = argv[7];
    
  google::InitGoogleLogging(argv[0]);
  FLAGS_stderrthreshold = 0; // INFO: 0, WARNING: 1, ERROR: 2, FATAL: 3
  FLAGS_colorlogtostderr = 1;

  // Read configuration file for OKVIS
  std::string configFilename(config_okvis);

  okvis::ViParametersReader viParametersReader(configFilename);
  viParametersReader.getParameters(parameters);
   
  // Store output stuff (est trajectory, meshes computed by s8, dbow vocab)
  // Everything should be in a folder named utils in your ws
  boost::filesystem::path package(package_dir);
  package.remove_filename().remove_filename(); // Now path is your workspace

  std::string trajectoryDir = package.string() + "/utils";
  std::string meshesDir = package.string() + "/utils" + "/meshes";
  publisher.setMeshesPath(meshesDir); // Tell publisher where submap meshes are
  std::string dBowVocDir = package.string() + "/utils";


  // =============== DELETE OLD MESHES ===============

  boost::filesystem::path root(meshesDir);
  std::vector<boost::filesystem::path> paths; // Paths of all ply files in directory
  

  if (boost::filesystem::exists(root) && boost::filesystem::is_directory(root))
  {
      for (auto const & entry : boost::filesystem::recursive_directory_iterator(root))
      {
          if (boost::filesystem::is_regular_file(entry))
              paths.emplace_back(entry.path());
      }
  }

  for (int i = 0; i < paths.size(); i++)
  {
      boost::filesystem::remove(paths[i]); // delete ply
      // std::cout << "\n\n\n deleting file " << paths[i].string() << "\n\n\n";
  }

  // ======================================
  

  std::ifstream infile(dBowVocDir + "/small_voc.yml.gz");
  if (!infile.good()) {
    LOG(ERROR) << "DBoW2 vocabulary " << dBowVocDir << "/small_voc.yml.gz not found.";
  }

  // okvis_estimator = new okvis::ThreadedSlam(parameters, dBowVocDir);
  okvis_estimator = std::make_shared<okvis::ThreadedSlam>(parameters, dBowVocDir);
  
  // this is where we decide whether to run OKVIS in real time or not!
  // if it's set to true, we always wait for the processing queues in okvis to free up -> if hardware is slow: we accumulate unprocessed messages and okvis is behind current pose
  // if it's set to false, when the processing queues are full we start dropping stuff -> in this case okvis either works in real time or it doesnt work at all!
  okvis_estimator->setBlocking(false);


  // write logs
  std::string mode = "slam";
  if (!parameters.estimator.do_loop_closures) {
    mode = "vio";
  }

  // estimated trajectory directory
  // writer = std::make_shared<okvis::TrajectoryOutput>(trajectoryDir + "/okvis2-" + mode + "_trajectory.csv", false);

// =============== SUPEREIGHT ===============

  // Get all the supereight related settings from a file
  const se::MapConfig mapConfig(config_s8);
  const se::OccupancyDataConfig dataConfig(config_s8);
  //const se::PinholeCameraConfig cameraConfig(config_s8);
  se::PinholeCameraConfig cameraConfig;
  cameraConfig.readYaml(config_s8);

  // depth cam extrinsics. in the visensor, the depth camera frame is the same as the left camera
  const Eigen::Matrix4d T_SC = parameters.nCameraSystem.T_SC(0)->T();

  publisher.setT_SC(T_SC);

  // Get submaps distance from config
  cv::FileStorage fs;
  fs.open(config_s8, cv::FileStorage::READ | cv::FileStorage::FORMAT_YAML);
  const cv::FileNode node = fs["submaps"];
  float distThreshold;
  se::yaml::subnode_as_float(node, "dist_threshold", distThreshold);
  assert(distThreshold >= 0);
  
  se_interface = std::make_shared<SupereightInterface>(cameraConfig, mapConfig, dataConfig, T_SC, meshesDir, static_cast<double>(distThreshold));
  
  // run in real time (not blocking) or not (blocking)?
  // while tracking should be real time, it's not relevant with depth integration
  // so you can also run it blocking
  se_interface->setBlocking(false);

  // =============== PLANNER ===============

  planner = std::make_shared<Planner>(se_interface.get(), config_s8);


  // =============== REGISTER CALLBACKS ===============

  // Set path visualizer
  planner->setPathCallback(std::bind(&Publisher::publishPathAsCallback, &publisher, std::placeholders::_1));

  // Send state to publisher, writer, planner. send state + graph (keyframes) to seinterface 
  okvis_estimator->setOptimisedGraphCallback([&](const okvis::State & _1, 
                             const okvis::TrackingState & _2,
                             std::shared_ptr<const okvis::AlignedVector<State>> _3,
                             std::shared_ptr<const okvis::MapPointVector> _4){ 

                              publisher.processState(_1,_2); 
                              publisher.publishKeyframesAsCallback(_1,_2,_3);
                              //  writer->processState(_1,_2,_3,_4); 
                              planner->processState(_1,_2);
                              se_interface->stateUpdateCallback(_1,_2,_3);});

  //// Sisualize the submaps
  // Mesh version:
  se_interface->setSubmapMeshesCallback(std::bind(&Publisher::publishSubmapMeshesAsCallback, &publisher, std::placeholders::_1));
  // Block version:
  // se_interface->setSubmapCallback(std::bind(&Publisher::publishSubmapsAsCallback, &publisher, std::placeholders::_1,std::placeholders::_2));

  // =============== REGISTER ROS CALLBACKS =============== 

  sync.registerCallback(boost::bind(&RosInterfacer::imgsCallback, this, _1, _2));

  navgoal_sub = nh.subscribe("/navgoal", 0, &RosInterfacer::navGoalCallback, this);

  imu_sub = nh.subscribe(std::string(imu_topic), 10000, &RosInterfacer::imuCallback, this);

  depth_sub = it.subscribe(std::string(depth_topic), 1000, &RosInterfacer::depthCallback, this);

}

RosInterfacer::~RosInterfacer()
{

}


int RosInterfacer::start()
{
  
  // =============== START THINGS UP ===============
  
  // Start supereight processing
  se_interface->start();

  thread_okvis = std::thread(&RosInterfacer::slam_loop,this);

  thread_okvis.detach();

  return 0;

}

void RosInterfacer::slam_loop(){

  std::cout << "\n\nStarting okvis processing... \n\n";

  while(ros::ok()){
  okvis_estimator->processFrame();
  // okvis_estimator->display();
  // writer->drawTopView();
  // se_interface->display();
  // cv::waitKey(2);
  }

}

void RosInterfacer::navGoalCallback(const geometry_msgs::Point &msg) 
{
  Eigen::Vector3d r(msg.x,msg.y,msg.z);
  
  planner->setGoal(r);

  // overloaded function -> does not work -> use lambda
  //thread_planner = std::thread(Planner::plan,planner.get(),r);
  thread_planner = std::thread([&](Eigen::Vector3d goal){planner->plan(goal);},r);
  thread_planner.detach();

}


void RosInterfacer::imuCallback(const sensor_msgs::ImuConstPtr& msg)
{
  // ROS_INFO("\n Sending IMU data to Okvis \n");

  okvis::Time t(msg->header.stamp.sec, msg->header.stamp.nsec);

  if(!okvis_estimator->addImuMeasurement( // pass imu meas. to okvis interface
      t,
      Eigen::Vector3d(msg->linear_acceleration.x, msg->linear_acceleration.y,
                      msg->linear_acceleration.z),
      Eigen::Vector3d(msg->angular_velocity.x, msg->angular_velocity.y,
                      msg->angular_velocity.z)))
                      {
                      LOG(WARNING) << "Imu meas. delayed at time "<<t;
                      }

}

void RosInterfacer::imgsCallback(const sensor_msgs::ImageConstPtr& img_0, const sensor_msgs::ImageConstPtr& img_1) 
{
  
  // ROS_INFO("\n Sending images to Okvis \n");

  std::vector<cv::Mat> img_vector;
  
  const cv::Mat raw_img_0(img_0->height, img_0->width, CV_8UC1,
                    const_cast<uint8_t*>(&img_0->data[0]), img_0->step);
  const cv::Mat raw_img_1(img_1->height, img_1->width, CV_8UC1,
                    const_cast<uint8_t*>(&img_1->data[0]), img_1->step);


  // Why is this necessary?
  // Im not even filtering!! but if I just take raw_img_
  // okvis starts generating a ton of kfs after some time

  cv::Mat filtered_img_0;
  cv::Mat filtered_img_1;

  if (false) { // filter
    cv::medianBlur(raw_img_0, filtered_img_0, 3);
    cv::medianBlur(raw_img_1, filtered_img_1, 3);
  } else { // no filter
    filtered_img_0 = raw_img_0.clone();
    filtered_img_1 = raw_img_1.clone();
  }
  
  // take timestamp from left img but they should be the same
  okvis::Time t(img_0->header.stamp.sec, img_0->header.stamp.nsec);
  t -= okvis::Duration(parameters.camera.image_delay);

  img_vector.push_back(filtered_img_0);
  img_vector.push_back(filtered_img_1);
  
  if (!okvis_estimator->addImages(t, img_vector))
    LOG(WARNING) << "Multiframe delayed at time "<<t;

}

void RosInterfacer::depthCallback(const sensor_msgs::ImageConstPtr& img){

  // TODO understand why this does not work w realsense (does work with uhumans dataset). 
  // gonna have to stop using cv bridge sooner or later

  // cv::Mat raw_depth(img->height, img->width, CV_32FC1, const_cast<uint8_t*>(&img->data[0]), img->step);
  // okvis::Time t(img->header.stamp.sec, img->header.stamp.nsec);
  // t -= okvis::Duration(parameters.camera.image_delay);

  // if (!se_interface->addDepthImage(t, raw_depth)) 
  //   LOG(WARNING) << "Depth frame delayed at time "<<t;

  // // cv::imshow("mydepth",raw_depth);
  // // cv::waitKey(2);

  cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(img, "32FC1");
  okvis::Time t(img->header.stamp.sec, img->header.stamp.nsec);
  t -= okvis::Duration(parameters.camera.image_delay);

  if (!se_interface->addDepthImage(t, cv_ptr->image))
    LOG(WARNING) << "Depth frame delayed at time "<<t;

}


int main(int argc, char** argv) 
{
  
  ros::init(argc, argv, "ros_interface");

  RosInterfacer node(argv);

  node.start(); // starts slam & mapping thread
  
  ros::spin();

  ros::shutdown();

  return 0;

}