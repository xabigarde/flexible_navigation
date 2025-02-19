/*******************************************************************************
 *  Copyright (c) 2016
 *  Capable Humanitarian Robotics and Intelligent Systems Lab (CHRISLab)
 *  Christopher Newport University
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *    3. Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 *       THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *       "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *       LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *       FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *       COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *       INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *       BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *       LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *       CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *       LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 *       WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *       POSSIBILITY OF SUCH DAMAGE.
 ******************************************************************************/

#include <base_local_planner/goal_functions.h>
#include <flex_nav_planners/follow_common.h>
#include <flex_nav_planners/follow_topic.h>
#include <geometry_msgs/Twist.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

namespace flex_nav {
FollowTopic::FollowTopic(tf2_ros::Buffer &tf)
    : tf_(tf), ft_server_(NULL), costmap_(NULL),
      loader_("nav_core", "nav_core::BaseGlobalPlanner"), running_(false),
      name_(ros::this_node::getName()) {
  ros::NodeHandle private_nh("~");
  ros::NodeHandle nh;

  ft_server_ = new FollowTopicActionServer(
      nh, name_, boost::bind(&FollowTopic::execute, this, _1), false);
  cc_server_ = new ClearCostmapActionServer(
      nh, name_ + "/clear_costmap",
      boost::bind(&FollowTopic::clear_costmap, this, _1), false);

  std::string planner;
  private_nh.param("planner", planner, std::string("navfn/NavfnROS"));
  private_nh.param("costmap_name", costmap_name_,
                   std::string("middle_costmap"));
  private_nh.param(costmap_name_ + "/robot_base_frame", robot_base_frame_,
                   std::string("base_link"));
  private_nh.param(costmap_name_ + "/reference_frame", global_frame_,
                   std::string("/odom"));
  private_nh.param("planner_frequency", planner_frequency_, 1.0);
  private_nh.param("distance_threshold", distance_threshold_, 5.0);

  costmap_ = new costmap_2d::Costmap2DROS(costmap_name_, tf);
  costmap_->pause();

  try {
    ROS_INFO("[%s] Create instance of %s planner", name_.c_str(),
             planner.c_str());
    planner_ = loader_.createInstance(planner);
    planner_->initialize(loader_.getName(planner), costmap_);
  } catch (const pluginlib::PluginlibException &ex) {
    ROS_ERROR("[%s] Failed to create the %s planner: %s", name_.c_str(),
              planner.c_str(), ex.what());
    exit(1);
  }

  costmap_->start();
  ft_server_->start();
  cc_server_->start();
  ros::spin();
}

FollowTopic::~FollowTopic() {
  ft_server_->shutdown();
  cc_server_->shutdown();
}

void FollowTopic::execute(
    const flex_nav_common::FollowTopicGoalConstPtr &goal) {
  ros::NodeHandle n;
  ros::Rate r(planner_frequency_);
  geometry_msgs::PoseStamped start;
  flex_nav_common::FollowTopicResult result;
  flex_nav_common::FollowTopicFeedback feedback;
  ros::Time start_time;

  while (running_) {
    ROS_WARN_THROTTLE(0.25, "[%s] Waiting for lock", name_.c_str());
    r.sleep();
  }

  current_path_.reset();
  latest_path_.reset();

  ROS_INFO("[%s] Attempting to listen to topic: %s", name_.c_str(),
           goal->topic.data.c_str());

  bool good(false);

  ros::master::V_TopicInfo master_topics;
  ros::master::getTopics(master_topics);

  for (ros::master::V_TopicInfo::iterator it = master_topics.begin();
       it != master_topics.end(); it++) {
    const ros::master::TopicInfo &info = *it;

    if (info.name.find(goal->topic.data) != std::string::npos &&
        !info.datatype.compare("nav_msgs/Path")) {
      sub_ = n.subscribe(goal->topic.data, 1, &FollowTopic::topic_cb, this);

      ROS_INFO("[%s] Success!", name_.c_str());
      good = true;
      break;
    }
  }

  // This is not good
  if (!good) {
    ROS_ERROR("[%s] Desired topic does not publish a nav_msgs/Path",
              name_.c_str());
    ft_server_->setAborted();
    sub_.shutdown();
    current_path_.reset();
    latest_path_.reset();
    return;
  }

  // Wait for a path to be received
  while ((!latest_path_ || latest_path_->poses.size() == 0) && n.ok() &&
         !ft_server_->isNewGoalAvailable() &&
         !ft_server_->isPreemptRequested()) {
    r.sleep();
  }

  result.pose = start.pose;

  running_ = true;
  while (running_ && n.ok() && !ft_server_->isNewGoalAvailable() &&
         !ft_server_->isPreemptRequested()) {
    current_path_ = latest_path_;

    if (current_path_->poses.empty()) {
      ROS_ERROR("[%s] The path is empty!", name_.c_str());
      return;
    }

    if (current_path_->poses[0].header.frame_id == "") {
      ROS_ERROR("[%s] The frame_id is empty!", name_.c_str());
      return;
    }

    start_time = ros::Time::now();
    geometry_msgs::PoseStamped transformed;

    geometry_msgs::PoseStamped pose;
    geometry_msgs::PoseStamped datTFPose;
    geometry_msgs::PoseStamped transformed_pose;
    costmap_->getRobotPose(pose); // odom frame

    tf_.transform(pose, transformed_pose, current_path_->poses[0].header.frame_id);

    ROS_DEBUG("[%s] Generating path from path: #%u", name_.c_str(),
              current_path_->header.seq);

    // Do some work to find the goal point
    geometry_msgs::PoseStamped goal_pose;

    costmap_2d::Costmap2D *costmap = costmap_->getCostmap();
    double r2 =
        std::min(costmap->getSizeInCellsX() * costmap->getResolution() / 2.0,
                 costmap->getSizeInCellsY() * costmap->getResolution() / 2.0) -
        costmap->getResolution() * 2;
    r2 = r2 * r2;

    if (!getTargetPointFromPath(r2, transformed_pose, current_path_->poses,
                                goal_pose)) {
      ROS_ERROR("[%s] No valid point found", name_.c_str());
      ROS_ERROR("[%s] Could not get a valid goal", name_.c_str());
      result.code = flex_nav_common::FollowTopicResult::FAILURE;
      ft_server_->setAborted(result, "Failed to get a valid goal");
      running_ = false;
      return;
    }

    tf_.transform(transformed_pose, transformed, current_path_->poses[0].header.frame_id);
    transformed = transformed_pose;

    feedback.pose = start.pose;
    ft_server_->publishFeedback(feedback);

    std::vector<geometry_msgs::PoseStamped> plan;
    if (planner_->makePlan(start, transformed, plan)) {
      if (plan.empty()) {
        result.code = flex_nav_common::FollowTopicResult::FAILURE; // empty plan
        ft_server_->setAborted(result, "Empty path");
        running_ = false;
        return;
      }
    } else {
      result.code =
          flex_nav_common::FollowTopicResult::FAILURE; // failed to make plan
      ft_server_->setAborted(result, "Failed to make plan");
      running_ = false;
      return;
    }

    double threshold = distance_threshold_ * costmap->getResolution();
    if (distanceSquared(start, transformed) <= threshold * threshold) {
      result.code = flex_nav_common::FollowTopicResult::SUCCESS;
      ft_server_->setSucceeded(result, "Success!");
      running_ = false;
      return;
    }

    r.sleep();
  }

  if (ft_server_->isPreemptRequested()) {
    ROS_WARN("[%s] Preempting goal...", name_.c_str());
    ft_server_->setPreempted(result, "Goal preempted");
  } else {
    ft_server_->setSucceeded(result, "Goal canceled");
  }
  running_ = false;
}

void FollowTopic::topic_cb(const nav_msgs::PathConstPtr &data) {
  ROS_DEBUG("[%s] Recieved a new path with %lu points: #%u", name_.c_str(),
            data->poses.size(), data->header.seq);

  latest_path_ = data;
}

void FollowTopic::clear_costmap(
    const flex_nav_common::ClearCostmapGoalConstPtr &goal) {
  costmap_->resetLayers();

  flex_nav_common::ClearCostmapResult result;
  result.code = flex_nav_common::ClearCostmapResult::SUCCESS;
  cc_server_->setSucceeded(result, "Success");
}
}
