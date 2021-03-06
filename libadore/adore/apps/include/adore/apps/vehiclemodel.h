/********************************************************************************
 * Copyright (C) 2017-2020 German Aerospace Center (DLR). 
 * Eclipse ADORe, Automated Driving Open Research https://eclipse.org/adore
 *
 * This program and the accompanying materials are made available under the 
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0 
 *
 * Contributors: 
 *   Daniel Heß - initial implementation
 ********************************************************************************/

#pragma once
#include <adore/sim/afactory.h>
#include <adore/params/afactory.h>
#include <adore/fun/ctrl/vlb_openloop.h>
#include <adore/mad/oderk4.h>
#include <adore/mad/adoremath.h>
#include <iostream>

namespace adore
{
    /**
     * @brief abstraction of functional modules to define functionality decoupled from the middleware
     * 
     */
    namespace apps
    {
        /**
         * @brief a vehicle model which can be used in simulations
         * 
         * needs a timer, control_input, gps_output from adore::sim
         * listens to reset_pose_feed and reset_twist_feed to reset pose/twist if requested
         * uses run()-function to update
         * 
         * @see adore::params::APVehicle on how to configure the vehicle model
         * @see VehicleModel::run()
         */
        class VehicleModel//@TODO: public AFixedRateApp - derive from process template which controls execution-timing during non-realtime simulation 
        {
            private:
                params::APVehicle* params_;
                double last_time_;
                double integration_step_;
                bool last_time_valid_;
                adoreMatrix<double,10,1> x_;
                adore::fun::MotionCommand u_;
                adore::mad::AReader<double>* timer_;
                adore::mad::AReader<adore::fun::MotionCommand>* control_input_; 
                adore::mad::AWriter<adore::fun::VehicleMotionState9d>* gps_output_; /** < publishes ego state measurement inside vehicle */
                adore::mad::OdeRK4<double> solver_;
                adore::mad::AFeed<adore::sim::ResetVehiclePose>* reset_pose_feed_;
                adore::mad::AFeed<adore::sim::ResetVehicleTwist>* reset_twist_feed_;  
                adore::sim::AFactory::TParticipantWriter* participant_writer_;/** < publishes vehicle state globally*/             
                int simulationID_;


            public:
                /**
                 * @brief Construct a new Vehicle Model object
                 *  
                 * @param sim_factory adore::sim factory
                 * @param paramfactory adore::params factory
                 * @param simulationID id of vehicle in simulation
                 */
                VehicleModel(sim::AFactory* sim_factory,params::AFactory* paramfactory,int simulationID)
                {
                    simulationID_ = simulationID;
                    integration_step_ = 0.005;
                    last_time_valid_ = false;
                    x_ = dlib::zeros_matrix<double>(10l,1l);
                    timer_ = sim_factory->getSimulationTimeReader();
                    control_input_ = sim_factory->getMotionCommandReader();
                    gps_output_ = sim_factory->getVehicleMotionStateWriter();
                    reset_pose_feed_ = sim_factory->getVehiclePoseResetFeed();
                    reset_twist_feed_ = sim_factory->getVehicleTwistResetFeed();
                    participant_writer_ = sim_factory->getParticipantWriter();
                    params_ = paramfactory->getVehicle();
                }
                /**
                 * @brief simulation step of the vehicle model
                 * 
                 */
                virtual void run()
                {
                    if( timer_->hasData() )
                    {

                        if( reset_pose_feed_->hasNext() )
                        {
                            adore::sim::ResetVehiclePose pose;
                            reset_pose_feed_->getLatest(pose);
                            x_(0) = pose.getX();
                            x_(1) = pose.getY();
                            x_(2) = pose.getPSI();

                            x_(3) = 0.0;
                            x_(4) = 0.0;
                            x_(5) = 0.0;
                            x_(6) = 0.0;
                            x_(7) = 0.0;
                            x_(8) = 0.0;
                            x_(9) = 0.0;
                        }
                        if( reset_twist_feed_->hasNext() )
                        {
                            adore::sim::ResetVehicleTwist twist;
                            reset_twist_feed_->getLatest(twist);
                            x_(3) = twist.getVx();
                            x_(4) = twist.getVy();
                            x_(5) = twist.getOmega();
                            x_(6) = 0.0;
                            x_(7) = 0.0;
                            x_(8) = 0.0;
                            x_(9) = 0.0;
                        }

                        double current_time;
                        timer_->getData(current_time);
                        if(last_time_valid_)
                        {
                            if( control_input_->hasData() )
                            {
                                control_input_->getData(u_);
                            }
                            else
                            {
                                u_.setAcceleration(0.0);
                                u_.setSteeringAngle(0.0);
                            }
                            auto T = adore::mad::sequence(last_time_,integration_step_,current_time);
                            
                            x_(6) = u_.getAcceleration();
                            x_(7) = u_.getSteeringAngle();
                            x_(8) = 0.0; //dax=0
                            x_(9) = 0.0; //ddelta=0
                            adore::fun::VLB_OpenLoop model(params_);
                            auto X = solver_.solve(&model,T,x_);
                            x_ = dlib::colm(X,X.nc()-1);
                            
                            //vehicle state measurement
                            adore::fun::VehicleMotionState9d xout;
                            xout.setX(x_(0));
                            xout.setY(x_(1));
                            xout.setZ(0.0);
                            xout.setPSI(x_(2));
                            xout.setvx(x_(3));
                            xout.setvy(x_(4));
                            xout.setOmega(x_(5));
                            xout.setAx(x_(6));
                            xout.setDelta(x_(7));
                            xout.setTime(current_time);
                            gps_output_->write(xout);
                            
                            //publication of vehicle state to other vehicles
                            adore::env::traffic::Participant yout;
                            double length = params_->get_a()+params_->get_b()+params_->get_c()+params_->get_d();
                            double distance_to_center = length*0.5-params_->get_d();
                            yout.center_(0) = xout.getX() + std::cos(xout.getPSI())*distance_to_center;
                            yout.center_(1) = xout.getY() + std::sin(xout.getPSI())*distance_to_center;
                            yout.center_(2) = xout.getZ();
                            yout.classification_ = adore::env::traffic::Participant::EClassification::CAR;
                            yout.classification_certainty_ = 1.0;
                            yout.existance_certainty_ = 1.0;
                            yout.acceleration_x_ = xout.getAx();
                            yout.vx_ = xout.getvx();
                            yout.vy_ = xout.getvy() + xout.getOmega() * distance_to_center;
                            yout.yawrate_ = xout.getOmega();
                            yout.observation_time_ = current_time;
                            yout.height_ = 1.8;
                            yout.length_ = length;
                            yout.width_ = params_->get_bodyWidth();
                            yout.yaw_ = xout.getPSI();
                            yout.trackingID_ = simulationID_;
                            yout.v2xStationID_ = simulationID_;//@TODO: fix this
                            participant_writer_->write(yout);

                            last_time_ = current_time;
                        }
                        else
                        {
                            last_time_ = current_time;
                            last_time_valid_ = true;
                        }
                    }
                }

        };
    }
}