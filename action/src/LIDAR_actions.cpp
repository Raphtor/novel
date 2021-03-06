#include <ros/ros.h>
#include <move_base_msgs/MoveBaseAction.h>
#include <actionlib/client/simple_action_client.h>
#include <novel_msgs/NovelObject.h>
#include <novel_msgs/NovelObjectArray.h>
#include <ar_track_alvar_msgs/AlvarMarkers.h>
#include <queue>
#include <nav_msgs/MapMetaData.h>
#include <nav_msgs/OccupancyGrid.h>
#include <math.h>
#include <cstdlib>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <std_msgs/Int8.h>
#include <tf/transform_listener.h>
#include <tf/transform_datatypes.h>

typedef actionlib::SimpleActionClient<move_base_msgs::MoveBaseAction> MoveBaseClient;

class LIDAR_Action {
  public:
    LIDAR_Action(tf::TransformListener& listener_, MoveBaseClient& ac_);
    void approach(ros::Time stamp);

  private:
    void push_to_queue(const novel_msgs::NovelObjectArray::ConstPtr& msg);
    void map_callback(const nav_msgs::OccupancyGrid::ConstPtr& msg);
    void pose_callback(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg);
    void state_callback(const std_msgs::Int8::ConstPtr& msg);
    void putObjInMap(double obj_x, double obj_y);

    ros::NodeHandle nh_;
    ros::Subscriber det_sub;
    ros::Subscriber map_sub;
    ros::Subscriber pose_sub;
    ros::Subscriber state_sub;
    tf::TransformListener &listener;
    ros::Publisher map_pub;
    ros::Publisher state_pub;
    MoveBaseClient& ac;
    // static tf::TransformListener listener;

    nav_msgs::MapMetaData map_metadata;
    float map_resolution;
    std::vector<signed char, std::allocator<signed char> > grid;
    
    geometry_msgs::PoseWithCovariance pose;
    
    bool map_known;
    bool pose_known;
    bool on;
        
    double min_marker_det_dist;

    std::queue< std::vector<double> > LIDAR_candidates;
};

LIDAR_Action::LIDAR_Action(tf::TransformListener& listener_, MoveBaseClient& ac_) :
  listener(listener_),
  ac(ac_)
 {
  map_known = false;
  pose_known = false;
  on = true;
  min_marker_det_dist = .75;
 

  std::string objs;
  nh_.getParam("/LIDAR_action/candidate_objs", objs);
  det_sub = nh_.subscribe<novel_msgs::NovelObjectArray>(objs, 1000, &LIDAR_Action::push_to_queue, this);
  map_sub = nh_.subscribe<nav_msgs::OccupancyGrid>("map", 1, &LIDAR_Action::map_callback, this);
  pose_sub = nh_.subscribe<geometry_msgs::PoseWithCovarianceStamped>("amcl_pose", 1, &LIDAR_Action::pose_callback, this);

  map_pub = nh_.advertise<nav_msgs::OccupancyGrid>("map", 1);
  state_pub = nh_.advertise<std_msgs::Int8>("state", 1);
}

/*
Changes state value to indicate progression of finite state machine
0 - move based on LIDAR detected objects
1 - detect using kinect
2 - move based on kinect detected markers

Input
-----
msg: integer message

Output
------
*/
void LIDAR_Action::state_callback(const std_msgs::Int8::ConstPtr& msg) {
  if (msg->data == 0) {
    on = true;
  } else {
    on = false;
  }
}

/*
Pushes LIDAR detected objects into queue

Input
-----
msg: array of novel objects detected

Output
------
*/
void LIDAR_Action::push_to_queue(const novel_msgs::NovelObjectArray::ConstPtr& msg) {
  // convert Point msg to grid coordinate wrt map frame
  ROS_INFO("In callback");
  for (int i = 0; i < msg->detected_objects.size(); i++) {
    geometry_msgs::PoseStamped pose;
    geometry_msgs::PoseStamped pose_out;

    pose.header.frame_id = "/base_scan";
    pose.pose = msg->detected_objects[i].pose.pose;
    try {
      
      listener.waitForTransform("/map", "/base_scan", msg->header.stamp, ros::Duration(1.0));
      listener.transformPose("/map", msg->header.stamp, pose, "/base_scan", pose_out);
      
      double x = pose_out.pose.position.x;
      double y = pose_out.pose.position.y;
      ROS_INFO_STREAM(x);
      ROS_INFO_STREAM(y);
      
      //double x = msg->detected_objects[i].pose.pose.position.x;
      //double y = msg->detected_objects[i].pose.pose.position.y;
      
      int x_coord = (int)round((x - map_metadata.origin.position.x)/map_resolution);
      int y_coord = (int)round((y - map_metadata.origin.position.y)/map_resolution);

      if (sqrt( pow(pose.pose.position.x,2) + pow(pose.pose.position.y,2)) < 16) {
        putObjInMap(x_coord, y_coord); // testing purposes

        std::vector<double> coord = {x, y};
        LIDAR_candidates.push(coord);
      }
    } catch (tf::TransformException ex) {
      ROS_ERROR("Push to queue %s", ex.what());
      ros::Duration(1.0).sleep();
    }
  }

  if ((int)msg->detected_objects.size() > 0) {
    ROS_INFO("Approaching %d objects", (int)msg->detected_objects.size());
    approach(msg->header.stamp);
  }

  ROS_INFO_THROTTLE(5,"Detected objects %d", (int) LIDAR_candidates.size());
}

/*
Determines action to execute based on LIDAR objs detected

Inputs
------
msg: published NovelObjectArray

Output
------

*/
void LIDAR_Action::approach(ros::Time stamp) {
  // if we know pose of robot, plan is not currently executing, and msg of detected ids came after plan finished
  
  ROS_INFO("Ready to move");
  // ROS_INFO_STREAM(LIDAR_candidates.size());
  if (map_known && pose_known && on) {
    while (!LIDAR_candidates.empty()) {
      std::vector<double> loc = LIDAR_candidates.front();      

      double x_obj = loc[0];
      double y_obj = loc[1];

      // get robot coordinates with respect to map frame
      tf::StampedTransform transform;
      try {
        tf::TransformListener listener;
        listener.waitForTransform("/map", "/base_link", stamp, ros::Duration(1.0));
        listener.lookupTransform("/map", "/base_link", stamp, transform);
      } catch (tf::TransformException ex) {
        ROS_ERROR("Approach: %s", ex.what());
        ros::Duration(1.0).sleep();
      }

      double x = transform.getOrigin().x(); // robot coordinate in map frame
      double y = transform.getOrigin().y();
      int robot_x = (int)round( (x-map_metadata.origin.position.x)/map_resolution); // robot map grid coordinate
      int robot_y = (int)round( (y-map_metadata.origin.position.y)/map_resolution);
      double angle = atan2(y_obj - y, x_obj - x);

      // tell robot to move within __ meters of LIDAR-detected obj
      // turn on kinect and see if marker is detected
      //   if detected, kinect_action node will move robot
      //   if not detected, tell robot to move in a circle around object for better view of potential marker
      bool detect = false;
      move_base_msgs::MoveBaseGoal goal;

      goal.target_pose.header.frame_id = "map"; // goal is relative to this frame
      
      int i = 0;
      while (!detect && i < 1) { //8) {
        goal.target_pose.pose.position.x = x_obj - min_marker_det_dist * cos(angle);
        goal.target_pose.pose.position.y = y_obj - min_marker_det_dist * sin(angle);
        
        // TODO: check that path between goal and candidate obj is unobstructed

        goal.target_pose.pose.orientation.z = sin(angle/2.0);
        goal.target_pose.pose.orientation.w = cos(angle/2.0);
        goal.target_pose.header.stamp = ros::Time::now();
        ROS_INFO("Sending goal");
        ac.sendGoal(goal);

        ac.waitForResult();
        actionlib::SimpleClientGoalState actionstate = ac.getState();
        if (actionstate == actionlib::SimpleClientGoalState::SUCCEEDED) {
          // start kinect detection
          std_msgs::Int8 kinect_detect;
          kinect_detect.data = 1;
          state_pub.publish(kinect_detect);
         
          ros::Duration(2.0).sleep();
 
          ROS_INFO_STREAM("Detecting marker");
          boost::shared_ptr<ar_track_alvar_msgs::AlvarMarkers const> shared_arr;
          ar_track_alvar_msgs::AlvarMarkers arr;
          shared_arr = ros::topic::waitForMessage<ar_track_alvar_msgs::AlvarMarkers>("ar_pose_marker", nh_);
          if (shared_arr != NULL) {
            arr = *shared_arr;
            if (arr.markers.size() > 0) {
              ROS_INFO_STREAM("Marker detected");

              // wait till message received that action is done
              boost::shared_ptr<std_msgs::Int8 const> shared_int;
              std_msgs::Int8 integer;
              shared_int = ros::topic::waitForMessage<std_msgs::Int8>("moved", nh_, ros::Duration(10));
              if (shared_int==NULL){
                ROS_INFO_STREAM("Moved message timed out...");


              } else {
                integer = *shared_int;
                if (integer.data == 1) {
                  detect = true;
                  ROS_INFO_STREAM("Done with marker action");
                } else {
                  ROS_INFO_STREAM("Not the marker associated with the object detected.");
                }
              }
              ros::Duration(1.0).sleep();
            } else {
              ROS_INFO_STREAM("Marker not detected.");
            }
          }
        } else {
          ROS_INFO("Action state: %s", actionstate.text_.c_str());
          ROS_INFO_STREAM("Could not approach LIDAR-detected object. Skipping LIDAR action.");
        }

        angle = angle + 3.1415/4;
        i = i + 1;
      }

      LIDAR_candidates.pop();
    }
  }
}

/*
Stores map once it is received

Input
-----
msg: occupancy grid map message

Output
------

*/
void LIDAR_Action::map_callback(const nav_msgs::OccupancyGrid::ConstPtr& msg) {
  map_metadata = msg->info;
  grid = msg->data;
  map_resolution = msg->info.resolution;
  map_known = true;
}

/*
Stores pose of robot

Input
-----
msg: pose with covariance of robot sent by amcl

Output
------
*/
void LIDAR_Action::pose_callback(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg) {
  pose_known = true;
  pose = msg->pose;
}

/*
Testing purposes
*/
void LIDAR_Action::putObjInMap(double obj_x, double obj_y) { //, double robot_x, double robot_y) {
  nav_msgs::OccupancyGrid msg1;
  grid[ obj_y*map_metadata.width + obj_x ] = 51;
  grid[ obj_y*map_metadata.width + obj_x - 1 ] = 49;
  grid[ obj_y*map_metadata.width + obj_x + 1 ] = 49;
  grid[ (obj_y-1)*map_metadata.width + obj_x ] = 49;
  grid[ (obj_y+1)*map_metadata.width + obj_x ] = 49;
  msg1.data = grid;
  msg1.info = map_metadata;
  map_pub.publish(msg1);
}


int main(int argc, char** argv){
  ros::init(argc, argv, "action");
  tf::TransformListener listener;
  MoveBaseClient ac("move_base", true);
  while (!ac.waitForServer(ros::Duration(5.0))) {
    ROS_INFO("Waiting for move_base action server to come up");
  }
  LIDAR_Action la = LIDAR_Action(listener, ac);
  ros::spin();
}
