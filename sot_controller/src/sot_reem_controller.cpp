/*
 * Software License Agreement (Modified BSD License)
 *
 *  Copyright (c) 2013, PAL Robotics, S.L.
 *  Copyright (c) 2008, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of PAL Robotics, S.L. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/** \author Gennaro Raiola
 *   inspired on the sot_pr2 class written by Thomas Moulard, available here https://github.com/laas/sot_pr2.
 */

# include <sot_controller/sot_reem_controller.h>
# include <pluginlib/class_list_macros.h>

# include <strings.h>
# include <Python.h>
# include <dynamic_graph_bridge/ros_init.hh>
# include <dynamic_graph_bridge/RunCommand.h>

#include <boost/thread/thread.hpp>
#include <boost/thread/condition.hpp>

namespace sot_reem_controller {

const std::string SotReemController::LOG_PYTHON="/tmp/sot_reem_controller.out";

SotReemController::SotReemController():
    interpreter_ (),
    device_ (new SotReemDevice("robot_device")) {}

SotReemController::~SotReemController() {

    device_->stopThread();

    for (int i = 0; i < joints_.size(); ++i){
        ROS_INFO("Current joint_%d position: %f64\n",i+1,joints_[i].getPosition());
    }
}

void SotReemController::runPython(std::ostream& file, const std::string& command, dynamicgraph::Interpreter& interpreter)
{
    file << ">>> " << command << std::endl;
    std::string lerr(""),lout(""),lres("");
    interpreter.runCommand(command,lres,lout,lerr);
    if (lres != "None")
    {
        if (lres=="<NULL>")
        {
            file << lout << std::endl;
            file << "------" << std::endl;
            file << lerr << std::endl;

            std::string err("Exception catched during sot controller initialization, please check the log file: " + LOG_PYTHON);
            throw std::runtime_error(err);
        }
        else
            file << lres << std::endl;
    }
}

bool SotReemController::init(hardware_interface::PositionJointInterface *robot, ros::NodeHandle& root_nh, ros::NodeHandle& controller_nh)
{

    interpreter_.reset(new dynamicgraph::Interpreter(controller_nh));

    // Call startup
    try
    {
        std::ofstream aof(LOG_PYTHON.c_str());

        runPython (aof, "import sys, os", *interpreter_);
        runPython (aof, "pythonpath = os.environ['PYTHONPATH']", *interpreter_);
        runPython (aof, "path = []", *interpreter_);
        runPython (aof,
                   "for p in pythonpath.split(':'):\n"
                   "  if p not in sys.path:\n"
                   "    path.append(p)", *interpreter_);
        runPython (aof, "path.extend(sys.path)", *interpreter_);
        runPython (aof, "sys.path = path", *interpreter_);
        runPython (aof, "sys.argv = 'reem'", *interpreter_);
        runPython(aof,"import startup", *interpreter_);

        aof.close();

        interpreter_->startRosService();
    }

    catch(const std::runtime_error& e)
    {
        ROS_ERROR_STREAM(e.what());
        return false;
    }

    // Get joint names from the parameter server
    using namespace XmlRpc;
    XmlRpcValue joint_names;
    ros::NodeHandle nh;
    std::string topicName = "jrl_map";
    if (!nh.getParam(topicName, joint_names)) //TODO: root_nh or a new global named node?
    {
        ROS_ERROR_STREAM("No joints given (expected namespace: /" + topicName + ").");
        return false;
    }
    if (joint_names.getType() != XmlRpcValue::TypeArray)
    {
        ROS_ERROR_STREAM("Malformed joint specification (namespace: /" + topicName + ").");
        return false;
    }

    std::vector<hardware_interface::JointHandle> joints_tmp; // Temporary container of joint handles

    joints_tmp.resize(joint_names.size());
    std::fill(joints_tmp.begin(),joints_tmp.end(),hardware_interface::JointHandle());
    for (int i = 0; i < joint_names.size(); ++i)
    {
        XmlRpcValue &name_value = joint_names[i];
        if (name_value.getType() != XmlRpcValue::TypeString)
        {
            ROS_ERROR_STREAM("Array of joint names should contain all strings (namespace:" << nh.getNamespace() << ").");
            return false;
        }
        const std::string joint_name = static_cast<std::string>(name_value);

        // Get a joint handle
        try
        {
            joints_tmp[i] = robot->getHandle(joint_name);

            ROS_DEBUG_STREAM("Found joint '" << joint_name << "' in the '" <<
                             getHardwareInterfaceType() << "' hardware interface.");
        }
        catch (...)
        {
            ROS_ERROR_STREAM("Could not find joint '" << joint_name << "' in the '" <<
                             getHardwareInterfaceType() << "' hardware interface.");
            return false;
        }
    }

    // Member list of joint handles is updated only once all resources have been claimed
    joints_ = joints_tmp;

    // Call init inside sot_reem_device
    device_->init();

    return true;
}

void SotReemController::starting(const ros::Time& time) {

    try{
        device_->starting(joints_);
    }
    catch(const std::range_error& e)
    {
        ROS_ERROR_STREAM(e.what());
    }

    device_->startThread(); // TODO: Should be moved in init

}

void SotReemController::update(const ros::Time& time, const ros::Duration& period) {

    device_->runDevice(period);

    ml::Vector state = device_->getState();
    for (unsigned int i = 0; i<joints_.size(); i++)
        joints_[i].setCommand(state(i));

}


}// namespace

PLUGINLIB_DECLARE_CLASS(sot_reem_controller,
                        SotReemController,
                        sot_reem_controller::SotReemController,
                        controller_interface::ControllerBase)
