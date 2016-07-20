/*
 * Copyright (C) 2016 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>
#include <gazebo/common/Assert.hh>
#include <gazebo/common/Console.hh>
#include <gazebo/common/Events.hh>
#include <gazebo/math/Pose.hh>
#include <gazebo/physics/PhysicsTypes.hh>
#include <gazebo/physics/World.hh>
#include <ros/ros.h>
#include <sdf/sdf.hh>

#include "osrf_gear/AriacTaskManagerPlugin.hh"
#include "osrf_gear/Goal.h"
#include "osrf_gear/Kit.h"
#include "osrf_gear/KitObject.h"

namespace gazebo
{
  /// \internal
  /// \brief Private data for the AriacTaskManagerPlugin class.
  struct AriacTaskManagerPluginPrivate
  {
    /// \brief World pointer.
    public: physics::WorldPtr world;

    /// \brief SDF pointer.
    public: sdf::ElementPtr sdf;

    /// \brief Class to store information about each object contained in a kit.
    public: class KitObject
            {
              /// \brief Stream insertion operator.
              /// \param[in] _out output stream.
              /// \param[in] _obj Kit object to output.
              /// \return The output stream
              public: friend std::ostream &operator<<(std::ostream &_out,
                                                      const KitObject &_obj)
              {
                _out << "<object>" << std::endl;
                _out << "Type: [" << _obj.type << "]" << std::endl;
                _out << "Pose: [" << _obj.pose << "]" << std::endl;
                _out << "</object>" << std::endl;
                return _out;
              }

               /// \brief Object type.
               public: std::string type;

               /// \brief Pose in which the object should be placed.
               public: math::Pose pose;
            };

    /// \brief Class to store information about a kit.
    public: class Kit
            {
              /// \brief Stream insertion operator.
              /// \param[in] _out output stream.
              /// \param[in] _kit kit to output.
              /// \return The output stream.
              public: friend std::ostream &operator<<(std::ostream &_out,
                                                      const Kit &_kit)
              {
                _out << "<kit>" << std::endl;
                for (auto obj : _kit.objects)
                  _out << obj << std::endl;
                _out << "/<kit>" << std::endl;

                return _out;
              }

              /// \brief A kit is composed by multiple objects.
              public: std::vector<KitObject> objects;
           };

    /// \brief Class to store information about a goal.
    public: class Goal
            {
              /// \brief Less than operator.
              /// \param[in] _goal Other goal to compare.
              /// \return True if this < _goal.
              public: bool operator<(const Goal &_goal) const
              {
                return this->time < _goal.time;
              }

              /// \brief Stream insertion operator.
              /// \param[in] _out output stream.
              /// \param[in] _goal Goal to output.
              /// \return The output stream.
              public: friend std::ostream &operator<<(std::ostream &_out,
                                                      const Goal &_goal)
              {
                _out << "<Goal>" << std::endl;
                _out << "Time: [" << _goal.time << "]" << std::endl;
                _out << "Kits:" << std::endl;
                for (auto kit : _goal.kits)
                  _out << kit << std::endl;
                _out << "</goal>" << std::endl;

                return _out;
              }

              /// \brief Simulation time in which the goal should be triggered.
              public: double time;

              /// \brief A goal is composed by multiple kits.
              public: std::vector<Kit> kits;
            };

    /// \brief Collection of goals.
    public: std::vector<Goal> goals;

    /// \brief ROS node handle.
    public: std::unique_ptr<ros::NodeHandle> rosnode;

    /// \brief Publishes a goal.
    public: ros::Publisher goalPub;

    /// \brief Connection event.
    public: event::ConnectionPtr connection;

    /// \brief The time specified in the object is relative to this time.
    public: common::Time startTime;
  };
}

using namespace gazebo;

GZ_REGISTER_WORLD_PLUGIN(AriacTaskManagerPlugin)

/////////////////////////////////////////////////
static void fillGoalMsg(const AriacTaskManagerPluginPrivate::Goal &_goal,
                        osrf_gear::Goal &_msgGoal)
{
  for (const auto &kit : _goal.kits)
  {
    osrf_gear::Kit msgKit;
    for (const auto &obj : kit.objects)
    {
      osrf_gear::KitObject msgObj;
      msgObj.type = obj.type;
      msgObj.target.position.x = obj.pose.pos.x;
      msgObj.target.position.y = obj.pose.pos.y;
      msgObj.target.position.z = obj.pose.pos.z;
      msgObj.target.orientation.x = obj.pose.rot.x;
      msgObj.target.orientation.y = obj.pose.rot.y;
      msgObj.target.orientation.z = obj.pose.rot.z;
      msgObj.target.orientation.w = obj.pose.rot.w;

      // Add the object to the kit.
      msgKit.objects.push_back(msgObj);
    }
    _msgGoal.kits.push_back(msgKit);
  }
}

/////////////////////////////////////////////////
AriacTaskManagerPlugin::AriacTaskManagerPlugin()
  : dataPtr(new AriacTaskManagerPluginPrivate)
{
}

/////////////////////////////////////////////////
AriacTaskManagerPlugin::~AriacTaskManagerPlugin()
{
  this->dataPtr->rosnode->shutdown();
}

/////////////////////////////////////////////////
void AriacTaskManagerPlugin::Load(physics::WorldPtr _world,
  sdf::ElementPtr _sdf)
{
  GZ_ASSERT(_world, "AriacTaskManagerPlugin world pointer is NULL");
  GZ_ASSERT(_sdf, "AriacTaskManagerPlugin sdf pointer is NULL");
  this->dataPtr->world = _world;
  this->dataPtr->sdf = _sdf;

  std::string robotNamespace = "";
  if (_sdf->HasElement("robot_namespace"))
  {
    robotNamespace = _sdf->GetElement(
      "robot_namespace")->Get<std::string>() + "/";
  }

  std::string goalsTopic = "goals";
  if (_sdf->HasElement("goals_topic"))
    goalsTopic = _sdf->Get<std::string>("goals_topic");

  // Parse the time.
  if (!_sdf->HasElement("goal"))
  {
    gzerr << "Unable to find a <goal> element" << std::endl;
    return;
  }

  sdf::ElementPtr goalElem = _sdf->GetElement("goal");
  while (goalElem)
  {
    // Parse the time.
    if (!goalElem->HasElement("time"))
    {
      gzerr << "Unable to find <time> element in <goal>. Ignoring" << std::endl;
      goalElem = goalElem->GetNextElement("goal");
      continue;
    }
    sdf::ElementPtr timeElement = goalElem->GetElement("time");
    double time = timeElement->Get<double>();

    // Parse the kits.
    if (!goalElem->HasElement("kit"))
    {
      gzerr << "Unable to find <kit> element in <goal>. Ignoring" << std::endl;
      goalElem = goalElem->GetNextElement("goal");
      continue;
    }

    // Store all kits for a goal.
    std::vector<AriacTaskManagerPluginPrivate::Kit> kits;

    sdf::ElementPtr kitElem = goalElem->GetElement("kit");
    while (kitElem)
    {
      // Parse the objects inside the kit.
      if (!kitElem->HasElement("object"))
      {
        gzerr << "Unable to find <object> element in <kit>. Ignoring"
              << std::endl;
        kitElem = kitElem->GetNextElement("kit");
        continue;
      }

      AriacTaskManagerPluginPrivate::Kit kit;

      sdf::ElementPtr objectElem = kitElem->GetElement("object");
      while (objectElem)
      {
        // Parse the object type.
        if (!objectElem->HasElement("type"))
        {
          gzerr << "Unable to find <type> in object.\n";
          objectElem = objectElem->GetNextElement("object");
          continue;
        }
        sdf::ElementPtr typeElement = objectElem->GetElement("type");
        std::string type = typeElement->Get<std::string>();

        // Parse the object pose (optional).
        if (!objectElem->HasElement("pose"))
        {
          gzerr << "Unable to find <pose> in object.\n";
          objectElem = objectElem->GetNextElement("object");
          continue;
        }
        sdf::ElementPtr poseElement = objectElem->GetElement("pose");
        math::Pose pose = poseElement->Get<math::Pose>();

        // Add the object to the kit.
        AriacTaskManagerPluginPrivate::KitObject obj = {type, pose};
        kit.objects.push_back(obj);

        objectElem = objectElem->GetNextElement("object");
      }

      // Add a new kit to the collection of kits.
      kits.push_back(kit);

      kitElem = kitElem->GetNextElement("kit");
    }

    // Add a new goal.
    AriacTaskManagerPluginPrivate::Goal goal = {time, kits};
    this->dataPtr->goals.push_back(goal);

    goalElem = goalElem->GetNextElement("goal");
  }

  // Sort the goals.
  std::sort(this->dataPtr->goals.begin(), this->dataPtr->goals.end());

  // Debug output.
  // gzdbg << "Goals:" << std::endl;
  // for (auto goal : this->dataPtr->goals)
  //   gzdbg << goal << std::endl;

  // Initialize ROS for publishing goals in the future.
  this->dataPtr->rosnode.reset(new ros::NodeHandle(robotNamespace));
  this->dataPtr->goalPub = this->dataPtr->rosnode->advertise<
    osrf_gear::Goal>(goalsTopic, 1000);

  this->dataPtr->startTime = this->dataPtr->world->GetSimTime();

  this->dataPtr->connection = event::Events::ConnectWorldUpdateEnd(
    boost::bind(&AriacTaskManagerPlugin::OnUpdate, this));
}

/////////////////////////////////////////////////
void AriacTaskManagerPlugin::OnUpdate()
{
  this->ProcessGoals();
}

/////////////////////////////////////////////////
void AriacTaskManagerPlugin::ProcessGoals()
{
  if (this->dataPtr->goals.empty())
    return;

  // Check whether announce a new goal from the list.
  auto elapsed = this->dataPtr->world->GetSimTime() - this->dataPtr->startTime;
  if (elapsed.Double() >= this->dataPtr->goals.front().time)
  {
    auto goal = this->dataPtr->goals.front();
    gzdbg << "Announcing goal: " << goal << std::endl;

    osrf_gear::Goal msgGoal;
    fillGoalMsg(goal, msgGoal);
    this->dataPtr->goalPub.publish(msgGoal);

    this->dataPtr->goals.erase(this->dataPtr->goals.begin());
  }
}
