//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2019 QMCPACK developers.
//
// File developed by: Leon Otis, leon_otis@berkeley.edu University, University of California Berkeley
//		      Ye Luo, yeluo@anl.gov, Argonne National Laboratory
//
// File created by: Leon Otis, leon_otis@berkeley.edu University, University of California Berkeley
//////////////////////////////////////////////////////////////////////////////////////

//Code for a descent engine

#include <cmath>
#include <vector>
#include <string>
#include "QMCDrivers/Optimizers/DescentEngine.h"
#include "OhmmsData/ParameterSet.h"
#include "Message/CommOperators.h"

namespace qmcplusplus
{
DescentEngine::DescentEngine(Communicate* comm, const xmlNodePtr cur)
    : my_comm_(comm),
      engine_target_excited_(false),
      num_params_(0),
      flavor_("RMSprop"),
      tjf_2body_eta_(.01),
      tjf_1body_eta_(.01),
      f_eta_(.001),
      gauss_eta_(.001),
      ci_eta_(.01),
      orb_eta_(.001),
      ramp_eta_(false),
      ramp_num_(30),
      store_num_(5),
      omega_(0.0),
      collection_step_(-1),
      compute_step_(-1),
      print_deriv_("no")
{
  descent_num_ = 0;
  store_count_ = 0;
  processXML(cur);
}


bool DescentEngine::processXML(const xmlNodePtr cur)
{
  std::string excited("no");
  std::string ramp_eta_str("no");

  ParameterSet m_param;
  m_param.add(excited, "targetExcited", "string");
  m_param.add(omega_, "omega", "double");
  app_log() << "Omega from input file: " << omega_ << std::endl;
  //Type of descent method being used
  m_param.add(flavor_, "flavor", "string");
  m_param.add(tjf_2body_eta_, "TJF_2Body_eta", "double");
  m_param.add(tjf_1body_eta_, "TJF_1Body_eta", "double");
  m_param.add(f_eta_, "F_eta", "double");
  m_param.add(ci_eta_, "CI_eta", "double");
  m_param.add(gauss_eta_, "Gauss_eta", "double");
  m_param.add(orb_eta_, "Orb_eta", "double");
  m_param.add(ramp_eta_str, "Ramp_eta", "string");
  m_param.add(ramp_num_, "Ramp_num", "int");
  m_param.add(store_num_, "Stored_Vectors", "int");
  m_param.add(print_deriv_,"Print_Deriv_Pieces","string");

  app_log() << "Current collection step: " << collection_step_ << std::endl;
  m_param.add(collection_step_,"collection_step","int");
  m_param.add(compute_step_,"compute_step","int");

  //Use -1 as a default value when you don't collect history. Would want to collect only during the descent finalization section
  if(collection_step_ != -1)
  {
      app_log() << "On descent finalization, have collect_count as true" << std::endl;
    collect_count_ = true;
  }

  m_param.put(cur);

  engine_target_excited_ = (excited == "yes");

  ramp_eta_ = (ramp_eta_str == "yes");


  return true;
}

//Prepare for taking samples to compute averaged derivatives
void DescentEngine::prepareStorage(const int num_replicas, const int num_optimizables)
{

    if(engine_target_excited_)
    {

   replica_numer_der_samp_.resize(num_replicas);
  replica_denom_der_samp_.resize(num_replicas); 


      avg_numer_der_samp_.resize(num_optimizables);
      avg_denom_der_samp_.resize(num_optimizables);


    
    std::fill(avg_numer_der_samp_.begin(), avg_numer_der_samp_.end(), 0.0);
    std::fill(avg_denom_der_samp_.begin(), avg_denom_der_samp_.end(), 0.0);

    for(int i = 0; i < num_replicas; i++)
    {

        replica_numer_der_samp_[i].resize(num_optimizables);
        replica_denom_der_samp_[i].resize(num_optimizables);

        std::fill(replica_numer_der_samp_[i].begin(), replica_numer_der_samp_[i].end(), 0.0);
        std::fill(replica_denom_der_samp_[i].begin(), replica_denom_der_samp_[i].end(), 0.0);

    }
    
    }

  avg_le_der_samp_.resize(num_optimizables);
  avg_der_rat_samp_.resize(num_optimizables);
  lderivs_.resize(num_optimizables);

  num_params_ = num_optimizables;

  std::fill(avg_le_der_samp_.begin(), avg_le_der_samp_.end(), 0.0);
  std::fill(avg_der_rat_samp_.begin(), avg_der_rat_samp_.end(), 0.0);

  replica_le_der_samp_.resize(num_replicas);
  replica_der_rat_samp_.resize(num_replicas);
  

  for (int i = 0; i < num_replicas; i++)
  {
    replica_le_der_samp_[i].resize(num_optimizables);
    std::fill(replica_le_der_samp_[i].begin(), replica_le_der_samp_[i].end(), 0.0);

    replica_der_rat_samp_[i].resize(num_optimizables);
    std::fill(replica_der_rat_samp_[i].begin(), replica_der_rat_samp_[i].end(), 0.0);

  }

  w_sum_        = 0;
  e_avg_        = 0;
  e_sum_        = 0;
  e_square_sum_ = 0;
  e_square_avg_ = 0;

  numer_avg_ = 0;
  numer_var_ = 0;
  denom_avg_ = 0;
  denom_var_ = 0;
  target_avg_ = 0;
  target_var_ = 0;

}



////////////////////////////////////////////////////////////////////////////////////////////////////////
/// \brief  Function that Take Sample Data from the Host Code
///
/// \param[in]  der_rat_samp   <n|Psi_i>/<n|Psi> (i = 0 (|Psi>), 1, ... N_var )
/// \param[in]  le_der_samp    <n|H|Psi_i>/<n|Psi> (i = 0 (|Psi>), 1, ... N_var )
/// \param[in]  ls_der_samp    <|S^2|Psi_i>/<n|Psi> (i = 0 (|Psi>), 1, ... N_var )
/// \param[in]  vgs_samp       |<n|value_fn>/<n|guiding_fn>|^2
/// \param[in]  weight_samp    weight for this sample
///
////////////////////////////////////////////////////////////////////////////////////////////////////////
void DescentEngine::takeSample(const int replica_id,
                               const std::vector<FullPrecValueType>& der_rat_samp,
                               const std::vector<FullPrecValueType>& le_der_samp,
                               const std::vector<FullPrecValueType>& ls_der_samp,
                               FullPrecValueType vgs_samp,
                               FullPrecValueType weight_samp)
{

  
  
    const size_t num_optimizables = der_rat_samp.size() - 1;

  ValueType etmp = le_der_samp.at(0);


  vgs_samp = static_cast<ValueType>(vgs_samp);

    lev_history_.push_back(etmp*vgs_samp);
    vg_history_.push_back(vgs_samp);
    w_history_.push_back(static_cast<ValueType>(1.0));

if(final_descent_num_ > collection_step_ && collect_count_ )
{
    
    final_lev_history_.push_back(etmp*vgs_samp);
    final_vg_history_.push_back(vgs_samp);
    final_w_history_.push_back(1.0);
}

  if(engine_target_excited_)
  {
      ValueType n;
      ValueType d;

        n = (omega_ - etmp)*vgs_samp;
        d = (omega_*omega_ - static_cast<ValueType>(2)*omega_*etmp + etmp*etmp)*vgs_samp;

      if(final_descent_num_ > collection_step_ && collect_count_)
      {
            final_tnv_history_.push_back(n);
            final_tdv_history_.push_back(d);
      
      }

    tnv_history_.push_back(n);
    tdv_history_.push_back(d);

 
    for(int i = 0; i < num_optimizables; i++)
    {
        //Combination of derivative ratios for target function numerator <psi | omega -H | psi>/<psi | psi>
        replica_numer_der_samp_[replica_id].at(i) += static_cast<ValueType>(2)*(omega_*der_rat_samp.at(i+1) -le_der_samp.at(i+1))*vgs_samp;
        
        //Combination of derivative ratios for target function denominator <psi | (omega -H)^2 | psi>/<psi | psi>
        replica_denom_der_samp_[replica_id].at(i) += static_cast<ValueType>(2)*((omega_*der_rat_samp.at(i+1) -le_der_samp.at(i+1))*(omega_*der_rat_samp.at(0) -le_der_samp.at(0)))*vgs_samp;

    }
 }

  for (int i = 0; i < num_optimizables; i++)
  {
    //replica_le_der_samp_[replica_id].at(i) += le_der_samp.at(i + 1)*vgs_samp*weight_samp;
    replica_le_der_samp_[replica_id].at(i) += le_der_samp.at(i + 1)*vgs_samp;
    //replica_der_rat_samp_[replica_id].at(i) += der_rat_samp.at(i + 1)*vgs_samp*weight_samp;
    replica_der_rat_samp_[replica_id].at(i) += der_rat_samp.at(i + 1)*vgs_samp;
  }

}

////////////////////////////////////////////////////////////////////////////////////////////////////////
/// \brief  Function that Take Sample Data from the Host Code
///
/// \param[in]  local_en       local energy
/// \param[in]  vgs_samp       |<n|value_fn>/<n|guiding_fn>|^2
/// \param[in]  weight_samp    weight for this sample
///
////////////////////////////////////////////////////////////////////////////////////////////////////////
//void DescentEngine::takeSample(RealType local_en, RealType vgs_samp, RealType weight_samp) {}

////////////////////////////////////////////////////////////////////////////////////////////////////////
/// \brief  Function that reduces all vector information from all processors to the root
///         processor
///
////////////////////////////////////////////////////////////////////////////////////////////////////////
void DescentEngine::sample_finish()
{

  
    int numSamples = lev_history_.size();

    this->mpi_unbiased_ratio_of_means(numSamples,w_history_,lev_history_,vg_history_,e_avg_,e_var_,e_err_);

    app_log() << "Energy Average: " << std::setprecision(9) << e_avg_ << std::endl;
    app_log() << "Energy Variance: " << std::setprecision(9) << e_var_ << std::endl;
    app_log() << "Weight Total: " << std::setprecision(9) << w_sum_ << std::endl;
    e_sd_ = std::sqrt(e_var_);
    app_log() << "Energy Standard Deviation: " << std::setprecision(9) << e_sd_ << std::endl;
    app_log() << "Energy Standard Error: " << std::setprecision(9) << e_err_ << std::endl;

    if(final_descent_num_ > collection_step_ && collect_count_ )
    {
        final_le_avg_history_.push_back(e_avg_);
        final_var_avg_history_.push_back(e_var_);
    
    }
    if(engine_target_excited_)
    {

 
        this->mpi_unbiased_ratio_of_means(numSamples,w_history_,tnv_history_,vg_history_,numer_avg_,numer_var_,numer_err_);
        
        this->mpi_unbiased_ratio_of_means(numSamples,w_history_,tdv_history_,vg_history_,denom_avg_,denom_var_,denom_err_);
        
        this->mpi_unbiased_ratio_of_means(numSamples,w_history_,tnv_history_,tdv_history_,target_avg_,target_var_,target_err_);
        
        app_log() << "Target Function Average: " << std::setprecision(9) << target_avg_ << std::endl;
        app_log() << "Target Function Variance: " << std::setprecision(9) << target_var_ << std::endl;
        app_log() << "Target Function Error: " << std::setprecision(9) << target_err_ << std::endl;

        if(final_descent_num_ > collection_step_ && collect_count_ )
        {
            final_tar_avg_history_.push_back(target_avg_);
            final_tar_var_history_.push_back(target_var_);
        }
        for(int i = 0; i < replica_numer_der_samp_.size(); i++)
        {
          for (int j = 0; j < lderivs_.size(); j++)
          {
          
          avg_numer_der_samp_[j] += replica_numer_der_samp_[i].at(j);
          avg_denom_der_samp_[j] += replica_denom_der_samp_[i].at(j);
          } 
        }   

  
  my_comm_->allreduce(avg_numer_der_samp_);
  my_comm_->allreduce(avg_denom_der_samp_);
  
    
    }

    
  for (int i = 0; i < replica_le_der_samp_.size(); i++)
  {
    for (int j = 0; j < lderivs_.size(); j++)
    {
      avg_le_der_samp_[j] += replica_le_der_samp_[i].at(j);
      avg_der_rat_samp_[j] += replica_der_rat_samp_[i].at(j);
    }
  }


  my_comm_->allreduce(avg_le_der_samp_);
  my_comm_->allreduce(avg_der_rat_samp_);

  int num_optimizables = lderivs_.size();

  //Vectors for parts of excited state functional derivatives
  std::vector<ValueType> numer_term1(num_optimizables,0.0);
  std::vector<ValueType> numer_term2(num_optimizables,0.0);
  std::vector<ValueType> denom(num_optimizables,0.0);

  ValueType gradNorm = 0.0;

  for (int i = 0; i < lderivs_.size(); i++)
  {
    avg_le_der_samp_.at(i)  = avg_le_der_samp_.at(i) / w_sum_;
    avg_der_rat_samp_.at(i) = avg_der_rat_samp_.at(i) / w_sum_;


    if(print_deriv_ == "yes")
    {
        app_log() << "Parameter # " << i << " Hamiltonian term: " << avg_le_der_samp_.at(i) << std::endl;
        app_log() << "Parameter # " << i << " Overlap term: " << avg_der_rat_samp_.at(i) << std::endl;
    }

    //Computation of averaged derivatives for excited state functional will be added in future
    if (!engine_target_excited_)
    {
      lderivs_.at(i) = 2.0 * (avg_le_der_samp_.at(i) - e_avg_ * avg_der_rat_samp_.at(i));
    //  app_log() << "Derivative for param # " << i << " : " << lderivs_.at(i) << std::endl;
    }
   
    else
    {
        

    avg_numer_der_samp_.at(i) = avg_numer_der_samp_.at(i)/w_sum_;
    avg_denom_der_samp_.at(i) = avg_denom_der_samp_.at(i)/w_sum_;

    if(print_deriv_ == "yes")
    {
        app_log() << "Parameter # " << i << " Numer Deriv: " << avg_numer_der_samp_.at(i) << std::endl;
        app_log()  << "Parameter # " << i << " Denom Deriv: " << avg_denom_der_samp_.at(i) << std::endl;
    }   
    
        numer_term1.at(i) = avg_numer_der_samp_.at(i)*denom_avg_;
        
        
           numer_term2.at(i) = avg_denom_der_samp_.at(i)*numer_avg_;

      
           denom.at(i) = denom_avg_*denom_avg_;
       
            lderivs_.at(i) = (numer_term1.at(i) - numer_term2.at(i))/denom.at(i);

        
    } 
  
    gradNorm += lderivs_.at(i)*lderivs_.at(i);  
  }

  gradNorm = std::sqrt(gradNorm);
    app_log() << "Norm of gradient vector is: " << gradNorm << std::endl;


    //Clear the history vectors for next iteration once sample_finish is done
    lev_history_.clear();
    vg_history_.clear();
    w_history_.clear();
    
    if(engine_target_excited_)
    {
        tnv_history_.clear();
        tdv_history_.clear();
    }

}


void DescentEngine::mpi_unbiased_ratio_of_means(int numSamples, std::vector<ValueType>& weights, std::vector<ValueType>& numerSamples,std::vector<ValueType>& denomSamples, ValueType& mean, ValueType& variance, ValueType& stdErr)
{
    std::vector<ValueType> y(7);
    y[0] = 0.0; // normalization constant
    y[1] = 0.0; // mean of numerator
    y[2] = 0.0; // mean of denominator
    y[3] = 0.0; // mean of the square of the numerator terms
    y[4] = 0.0; // mean of the square of the denominator terms
    y[5] = 0.0; // mean of the product of numerator times denominator
    y[6] = ValueType(numSamples); // number of samples

    for(int i = 0; i < numSamples ; i ++)
    {

        ValueType n = numerSamples[i];
        ValueType d = denomSamples[i];
        ValueType weight = weights[i];

        y[0] += weight;
        y[1] += weight*n;
        y[2] += d;
        y[3] += weight*n*n;
        y[4] += weight*d*d;
        y[5] += weight*n*d;

    }

    my_comm_->allreduce(y);

    ValueType mf = y[1] / y[0]; // mean of numerator
    ValueType mg = y[2] / y[0]; // mean of denominator
    ValueType sf = y[3] / y[0]; // mean of the square of the numerator terms
    ValueType sg = y[4] / y[0]; // mean of the square of the denominator terms
    ValueType mp = y[5] / y[0]; // mean of the product of numerator times denominator
    ValueType ns = y[6];        // number of samples


    ValueType vf = ( sf - mf * mf ) * ns / ( ns - 1.0 );
    ValueType vg = ( sg - mg * mg ) * ns / ( ns - 1.0 );
    ValueType cv = ( mp - mf * mg ) * ns / ( ns - 1.0 );

    w_sum_ = y[0];
    mean = ( mf / mg ) / ( 1.0 + ( vg / mg / mg - cv / mf / mg ) / ns ); 
    variance = ( mf * mf / mg / mg ) * ( vf / mf / mf + vg / mg / mg - 2.0 * cv / mf / mg );
    stdErr = std::sqrt(variance/ns);

}


//Function for updating parameters during descent optimization
void DescentEngine::updateParameters()
{
  app_log() << "Number of Parameters: " << num_params_ << std::endl;
app_log() << "Descent Number: " << descent_num_ << std::endl;

app_log() << "Finalization Descent Num (should be zero if not on last section): " << final_descent_num_ << std::endl;
if (final_descent_num_ > collection_step_ && collect_count_)
{
    app_log() << "Should be storing history, length of history on one process is: " << final_lev_history_.size() << std::endl;
}

  app_log() << "Parameter Type step sizes: "
            << " TJF_2Body_eta=" << tjf_2body_eta_ << " TJF_1Body_eta=" << tjf_1body_eta_ << " F_eta=" << f_eta_
            << " CI_eta=" << ci_eta_ << " Orb_eta=" << orb_eta_ << std::endl;

  // Get set of derivatives for current (kth) optimization step
  std::vector<ValueType> cur_deriv_set = deriv_records_.at(deriv_records_.size() - 1);
  std::vector<ValueType> prev_deriv_set;

  if (!taus_.empty())
  {
    // Get set of derivatives for previous (k-1th) optimization step
    prev_deriv_set = deriv_records_.at(deriv_records_.size() - 2);
  }

  ValueType denom;
  ValueType numer;
  ValueType v;
  ValueType cor_numer;
  ValueType cor_v;

  ValueType epsilon = 1e-8;
  ValueType type_eta = 0;

  ValueType tau = 0;
  // Update parameters according to specified flavor of gradient descent method

  // RMSprop corresponds to the method used by Booth and co-workers
  if (flavor_.compare("RMSprop") == 0)
  {
    app_log() << "Using RMSprop" << std::endl;

    // To match up with Booth group paper notation, prevLambda is lambda_k-1,
    // curLambda is lambda_k, nextLambda is lambda_k+1
    ValueType cur_lambda  = static_cast<ValueType>(.5) + static_cast<ValueType>(.5) * std::sqrt(static_cast<ValueType>(1.0) + static_cast<ValueType>(4.0) * lambda_ * lambda_);
    ValueType next_lambda = static_cast<ValueType>(.5) + static_cast<ValueType>(.5) * std::sqrt(static_cast<ValueType>(1.0) + static_cast<ValueType>(4.0) * cur_lambda * cur_lambda);
    ValueType gamma       = (static_cast<ValueType>(1.0) - cur_lambda) / next_lambda;

    // Define damping factor that turns off acceleration of the algorithm
    // small value of d corresponds to quick damping and effectively using
    // steepest descent
    ValueType d            = 100;
    ValueType decay_factor = std::exp(-(static_cast<ValueType>(1.0) / d) * (static_cast<ValueType>(descent_num_)));
    gamma                  = gamma * decay_factor;

    ValueType rho = .9;

    for (int i = 0; i < num_params_; i++)
    {
      ValueType cur_square = std::pow(cur_deriv_set.at(i), static_cast<RealType>(2));

      // Need to calculate step size tau for each parameter inside loop
      // In RMSprop, the denominator of the step size depends on a a running average of past squares of the parameter derivative
      if (derivs_squared_.size() < num_params_)
      {
        cur_square = std::pow(cur_deriv_set.at(i), static_cast<RealType>(2));
      }
      else if (derivs_squared_.size() >= num_params_)
      {
        cur_square = rho * derivs_squared_.at(i) + (static_cast<ValueType>(1.0) - rho) * std::pow(cur_deriv_set.at(i), static_cast<RealType>(2));
      }

      denom = std::sqrt(cur_square + epsilon);

      //The numerator of the step size is set according to parameter type based on input choices
      type_eta = this->setStepSize(i);
      //app_log() << "Type eta for parameter #" << i << ": " << type_eta << std::endl;
      tau      = type_eta / denom;

      // Include an additional factor to cause step size to eventually decrease to 0 as number of steps taken increases
      ValueType step_lambda = .1;

      ValueType step_decay_denom = static_cast<ValueType>(1.0) + step_lambda * static_cast<ValueType>(descent_num_);
      tau                        = tau / step_decay_denom;

       //  app_log() << "Final step size for parameter #" << i <<" is: " << tau << std::endl;
      //Update parameter values
      //If case corresponds to being after the first descent step
      if (taus_.size() >= num_params_)
      {
        ValueType old_tau = taus_.at(i);

        current_params_.at(i) = (static_cast<ValueType>(1.0) - gamma) * (current_params_.at(i) - tau * cur_deriv_set.at(i)) +
            gamma * (params_copy_.at(i) - old_tau * prev_deriv_set.at(i));
      }
      else
      {
        tau = type_eta;

        current_params_.at(i) = current_params_.at(i) - tau * cur_deriv_set.at(i);
      }

      if (taus_.size() < num_params_)
      {
        // For the first optimization step, need to add to the vectors
        taus_.push_back(tau);
        derivs_squared_.push_back(cur_square);
      }
      else
      {
        // When not on the first step, can overwrite the previous stored values
        taus_[i]           = tau;
        derivs_squared_[i] = cur_square;
      }

      params_copy_[i] = current_params_[i];
    }

    // Store current (kth) lambda value for next optimization step
    lambda_ = cur_lambda;
  }
  // Random uses only the sign of the parameter derivatives and takes a step of random size within a range.
  else if (flavor_.compare("Random") == 0)
  {
    app_log() << "Using Random" << std::endl;

    for (int i = 0; i < num_params_; i++)
    {
      denom           = 1;
      ValueType alpha = (static_cast<ValueType>(rand() / RAND_MAX));
      ValueType sign  = std::abs(cur_deriv_set[i]) / cur_deriv_set[i];
      if (std::isnan(std::real(sign)))
      {
        app_log() << "Got a nan, choosing sign randomly with 50-50 probability" << std::endl;

        ValueType t = (static_cast<ValueType>(rand() / RAND_MAX));
        if (std::real(t) > std::real(.5))
        {
          sign = 1;
        }
        else
        {
          sign = -1;
        }
      }
      app_log() << "This is random alpha: " << alpha << " with sign: " << sign << std::endl;

      current_params_.at(i) = current_params_.at(i) - tau * alpha * sign;
    }
  }

  else
  {
    // ADAM method
    if (flavor_.compare("ADAM") == 0)
    {
      app_log() << "Using ADAM" << std::endl;

      for (int i = 0; i < num_params_; i++)
      {
        ValueType cur_square = std::pow(cur_deriv_set.at(i), static_cast<RealType>(2));
        ValueType beta1      = .9;
        ValueType beta2      = .99;
        if (descent_num_ == 0)
        {
          numer_records_.push_back(0);
          denom_records_.push_back(0);
        }
        numer = beta1 * numer_records_[i] + (static_cast<ValueType>(1.0) - beta1) * cur_deriv_set[i];
        v     = beta2 * denom_records_[i] + (static_cast<ValueType>(1.0) - beta2) * cur_square;

        cor_numer = numer / (static_cast<ValueType>(1.0) - std::pow(beta1, static_cast<RealType>(descent_num_ + 1)));
        cor_v     = v / (static_cast<ValueType>(1.0) - std::pow(beta2, static_cast<RealType>(descent_num_ + 1)));

        denom = std::sqrt(cor_v) + epsilon;

        type_eta = this->setStepSize(i);
        tau      = type_eta / denom;

        current_params_.at(i) = current_params_.at(i) - tau * cor_numer;

        if (taus_.size() < num_params_)
        {
          // For the first optimization step, need to add to the vectors
          taus_.push_back(tau);
          derivs_squared_.push_back(cur_square);
          denom_records_[i] = v;
          numer_records_[i] = numer;
        }
        else
        {
          // When not on the first step, can overwrite the previous stored values
          taus_[i]           = tau;
          derivs_squared_[i] = cur_square;
          denom_records_[i]  = v;
          numer_records_[i]  = numer;
        }

        params_copy_[i] = current_params_.at(i);
      }
    }
    // AMSGrad method, similar to ADAM except for form of the step size denominator
    else if (flavor_.compare("AMSGrad") == 0)
    {
      app_log() << "Using AMSGrad" << std::endl;


      for (int i = 0; i < num_params_; i++)
      {
        ValueType cur_square = std::pow(cur_deriv_set.at(i), static_cast<RealType>(2));
        ValueType beta1      = .9;
        ValueType beta2      = .99;
        if (descent_num_ == 0)
        {
          numer_records_.push_back(0);
          denom_records_.push_back(0);
        }

        numer = beta1 * numer_records_[i] + (static_cast<ValueType>(1.0) - beta1) * cur_deriv_set[i];
        v     = beta2 * denom_records_[i] + (static_cast<ValueType>(1.0) - beta2) * cur_square;
        v     = std::max(std::real(denom_records_[i]), std::real(v));

        denom    = std::sqrt(v) + epsilon;
        type_eta = this->setStepSize(i);
        tau      = type_eta / denom;

        current_params_.at(i) = current_params_.at(i) - tau * numer;

        if (taus_.size() < num_params_)
        {
          // For the first optimization step, need to add to the vectors
          taus_.push_back(tau);
          derivs_squared_.push_back(cur_square);
          denom_records_[i] = v;
          numer_records_[i] = numer;
        }
        else
        {
          // When not on the first step, can overwrite the previous stored values
          taus_[i]           = tau;
          derivs_squared_[i] = cur_square;
          denom_records_[i]  = v;
          numer_records_[i]  = numer;
        }

        params_copy_[i] = current_params_.at(i);
      }
    }
  }


  descent_num_++;
  if(collect_count_)
  {
    final_descent_num_++;
    if(final_descent_num_ >= compute_step_)
    {
        app_log() << "Computing average energy over stored steps and its standard error" << std::endl;
        this->computeFinalizationAverages(final_w_history_,final_lev_history_,final_vg_history_);
        if(engine_target_excited_)
        {
            app_log() << "Computing average target function over stored steps and its standard error" << std::endl;
            this->computeFinalizationAverages(final_w_history_,final_tnv_history_,final_tdv_history_);
        }
    }
  }
}

// Helper method for setting step size according parameter type.
DescentEngine::ValueType DescentEngine::setStepSize(int i)
{
  ValueType type_eta;


  std::string name = engine_param_names_[i];


  int type = engine_param_types_[i];

  //Step sizes are assigned according to parameter type identified from the variable name.
  //Other parameter types could be added to this section as other wave function ansatzes are developed.
  if ((name.find("uu") != std::string::npos) || (name.find("ud") != std::string::npos))
  {
    type_eta = tjf_2body_eta_;
  }
  //If parameter name doesn't have "uu" or "ud" in it and is of type 1, assume it is a 1 body Jastrow parameter.
  else if (type == 1)
  {
    type_eta = tjf_1body_eta_;
  }
  else if (name.find("F_") != std::string::npos)
  {
    type_eta = f_eta_;
  }
  else if (name.find("CIcoeff_") != std::string::npos || name.find("CSFcoeff_") != std::string::npos)
  {
    type_eta = ci_eta_;
  }
  else if (name.find("orb_rot_") != std::string::npos)
  {
    type_eta = orb_eta_;
  }
  else if (name.find("g") != std::string::npos)
  {
    //Gaussian parameters are rarely optimized in practice but the descent code allows for it.
    type_eta = gauss_eta_;
  }
  else
  {
    //If there is some other parameter type that isn't in one of the categories with a default/input, use a conservative default step size.
    type_eta = .001;
  }

  
  if (ramp_eta_ && descent_num_ < ramp_num_)
  {
    type_eta = type_eta *static_cast<ValueType>((descent_num_ + 1) /(double) ramp_num_);
  }

  return type_eta;
}

//Method for retrieving parameter values, names, and types from the VariableSet before the first descent optimization step
void DescentEngine::setupUpdate(const optimize::VariableSet& my_vars)
{
    //omega_ = omega_input;

  num_params_ = my_vars.size();
  app_log() << "This is num_params_: " << num_params_ << std::endl;
  for (int i = 0; i < num_params_; i++)
  {
      app_log() << "Variable #" << i << ": " << my_vars[i] << " with index val: " << my_vars.where(i) << std::endl;
    if(my_vars.where(i) != -1)
    {
    engine_param_names_.push_back(my_vars.name(i));
    engine_param_types_.push_back(my_vars.getType(i));
    params_copy_.push_back(my_vars[i]);
    current_params_.push_back(my_vars[i]);
    params_for_diff_.push_back(my_vars[i]);
    }
  }
}

// Helper method for storing vectors of parameter differences over the course of
// a descent optimization for use in BLM steps of the hybrid method
void DescentEngine::storeVectors(std::vector<ValueType>& current_params)
{
  std::vector<ValueType> row_vec(current_params.size(), 0.0);

  // Take difference between current parameter values and the values from some number of
  // iterations before to be stored as input to BLM.
  // The current parameter values are then copied to params_for_diff_ to be used
  // if storeVectors is called again later in the optimization.
  for (int i = 0; i < current_params.size(); i++)
  {
    row_vec[i]          = current_params[i] - params_for_diff_[i];
    params_for_diff_[i] = current_params[i];
  }

  // If on first store of descent section, clear anything that was in vector
  if (store_count_ == 0)
  {
    hybrid_blm_input_.clear();
    hybrid_blm_input_.push_back(row_vec);
  }
  else
  {
    hybrid_blm_input_.push_back(row_vec);
  }

 #if !defined(QMC_COMPLEX)
  for (int i = 0; i < hybrid_blm_input_.size(); i++)
  {
    std::string entry = "";
    for (int j = 0; j < hybrid_blm_input_.at(i).size(); j++)
    {
      entry = entry + std::to_string(hybrid_blm_input_.at(i).at(j)) + ",";
    }
    app_log() << "Stored Vector: " << entry << std::endl;
  }
#endif
  store_count_++;
}



void DescentEngine::computeFinalizationAverages(std::vector<ValueType>& weights, std::vector<ValueType>& numerSamples,std::vector<ValueType>& denomSamples)
{


    
   //Make copies of input vectors to do recursive blocking for error estimates
    std::vector<ValueType> wtv(weights);
    std::vector<ValueType> nmv(numerSamples);
    std::vector<ValueType> dnv(denomSamples);


//Reproduce LM engine's crude way of estimating uncertainty in the variance
    int n = nmv.size();
    int blocks = 100;
   int section_len = n/100;

    std::vector<ValueType> bbvars(blocks);
ValueType bbv = 0.0;
ValueType bbv2 = 0.0;

    for(int i = 0; i < blocks; i++)
    {
        std::vector<ValueType> sub_wtv(section_len);
        std::vector<ValueType> sub_nmv(section_len);
        std::vector<ValueType> sub_dnv(section_len);

        ValueType temp_e, temp_v;
        std::copy(wtv.begin() + i*section_len,wtv.begin() + (i+1)*section_len,sub_wtv.begin());
        std::copy(nmv.begin() + i*section_len,nmv.begin() + (i+1)*section_len,sub_nmv.begin());
        std::copy(dnv.begin() + i*section_len,dnv.begin() + (i+1)*section_len,sub_dnv.begin());

//    app_log() << "First elements: " << sub_wtv[0] << " , " << sub_nmv[0] << " , " << sub_dnv[0] << std::endl;   
        ValueType blockVar; 
        ValueType blockMean;
        ValueType blockErr;

        
        this->mpi_unbiased_ratio_of_means(section_len,sub_wtv,sub_nmv,sub_dnv,blockMean,blockVar,blockErr);
        
        bbvars[i] = blockVar;

        bbv += bbvars.at(i) / static_cast<ValueType>(blocks);
        bbv2 += bbvars.at(i) * bbvars.at(i) / static_cast<ValueType>(blocks);
        sub_wtv.clear();
        sub_nmv.clear();
        sub_dnv.clear();

    }

    const ValueType bbvov = bbv2 - bbv * bbv;
    ValueType var_uncertainty = std::sqrt( std::abs(bbvov) / blocks);
    //Depending on when this function is called, this will be the uncertainty in the variance
    //of either the energy or the target function.
    //Which one should be clear from the preceeding print statements in the output file.
    app_log() << "Uncertainty in variance of averaged quantity: " << var_uncertainty << std::endl;


//To compute the standard error on the energy/target function itself, blocking is performed on the whole set of samples in the history of many iterations.
while(n > 128)
{
    ValueType currentVar;
    ValueType currentMean;
    ValueType currentErr;
    this->mpi_unbiased_ratio_of_means(n,wtv,nmv,dnv,currentMean,currentVar,currentErr);


    app_log() << "Blocking analysis error for per process length " << n << " is: " << currentErr << std::endl; 
    int new_len;
    if(n % 2 == 0)
    {
        new_len = n/2;
    }
    else
    {
        new_len = n/2 +1;
    }

    std::vector<ValueType> tmp1,tmp2,tmp3;

    for(int i = 0; i < n; i +=2)
    {
        ValueType avgW = (wtv[i]+wtv[i+1])/2.0;
        ValueType avgNumer = (nmv[i]+nmv[i+1])/2.0;
        ValueType avgDenom = (dnv[i]+dnv[i+1])/2.0;

        tmp1.push_back(avgW);
        tmp2.push_back(avgNumer);
        tmp3.push_back(avgDenom);

    }
    wtv = tmp1;
    nmv = tmp2;
    dnv = tmp3;
    n = nmv.size();
}


}



} // namespace qmcplusplus
