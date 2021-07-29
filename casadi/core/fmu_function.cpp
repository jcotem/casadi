/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "fmu_function.hpp"
#include "casadi_misc.hpp"
#include "serializing_stream.hpp"
#include "dae_builder_internal.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

namespace casadi {

#ifdef WITH_FMU

FmuFunction::FmuFunction(const std::string& name, const DaeBuilder& dae,
    const std::vector<std::vector<size_t>>& id_in,
    const std::vector<std::vector<size_t>>& id_out,
    const std::vector<std::string>& name_in,
    const std::vector<std::string>& name_out)
  : FunctionInternal(name), dae_(dae), id_in_(id_in), id_out_(id_out) {
  // Names of inputs
  if (!name_in.empty()) {
    casadi_assert(id_in.size() == name_in.size(), "Mismatching number of input names");
    name_in_ = name_in;
  }
  // Names of outputs
  if (!name_out.empty()) {
    casadi_assert(id_out.size() == name_out.size(), "Mismatching number of output names");
    name_out_ = name_out;
  }
}

FmuFunction::~FmuFunction() {
  // Free memory
  clear_mem();
}

void FmuFunction::init(const Dict& opts) {
  // Call the initialization method of the base class
  FunctionInternal::init(opts);

  // Get a pointer to the DaeBuilder class
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);

  // Cast id_in to right type
  vref_in_.resize(id_in_.size());
  for (size_t k = 0; k < id_in_.size(); ++k) {
    vref_in_[k].resize(id_in_[k].size());
    for (size_t i = 0; i < id_in_[k].size(); ++i)
      vref_in_[k][i] = dae->variable(id_in_[k][i]).value_reference;
  }
  // Cast id_out to right type
  vref_out_.resize(id_out_.size());
  for (size_t k = 0; k < id_out_.size(); ++k) {
    vref_out_[k].resize(id_out_[k].size());
    for (size_t i = 0; i < id_out_[k].size(); ++i)
      vref_out_[k][i] = dae->variable(id_out_[k][i]).value_reference;
  }

  // Load on first encounter
  if (dae->fmu_ == 0) dae->init_fmu();
}

int FmuFunction::init_mem(void* mem) const {
  if (FunctionInternal::init_mem(mem)) return 1;
  auto m = static_cast<FmuFunctionMemory*>(mem);

  // Get a pointer to the DaeBuilder class
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);

  // Create instance
  fmi2String instanceName = dae->model_identifier_.c_str();
  fmi2Type fmuType = fmi2ModelExchange;
  fmi2String fmuGUID = dae->guid_.c_str();
  fmi2String fmuResourceLocation = dae->fmu_->resource_loc_.c_str();
  fmi2Boolean visible = fmi2False;
  fmi2Boolean loggingOn = fmi2False;
  m->c = dae->fmu_->instantiate1();
  m->first_run = true;

  return 0;
}

void FmuFunction::free_mem(void *mem) const {
  auto m = static_cast<FmuFunctionMemory*>(mem);
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);
  if (dae->fmu_) dae->fmu_->free_instance1(m->c);
  delete m;
}

int FmuFunctionAdj::init_mem(void* mem) const {
  return derivative_of_->init_mem(mem);
}

void FmuFunctionAdj::free_mem(void *mem) const {
  return derivative_of_->free_mem(mem);
}

int FmuFunctionJac::init_mem(void* mem) const {
  return derivative_of_->init_mem(mem);
}

void FmuFunctionJac::free_mem(void *mem) const {
  return derivative_of_->free_mem(mem);
}

Sparsity FmuFunction::get_sparsity_in(casadi_int i) {
  return Sparsity::dense(id_in_.at(i).size(), 1);
}

Sparsity FmuFunction::get_sparsity_out(casadi_int i) {
  return Sparsity::dense(id_out_.at(i).size(), 1);
}

int FmuFunction::set_inputs(FmuFunctionMemory* m, const double** x) const {
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);
  // Reset solver
  if (m->first_run) {
    // Need to reset if time called again
    m->first_run = false;
  } else {
    // Reset solver
    fmi2Status status = dae->fmu_->reset_(m->c);
    if (status != fmi2OK) {
      casadi_warning("fmi2Reset failed");
      return 1;
    }
  }

  // Reset solver
  fmi2Status status = dae->fmu_->setup_experiment_(m->c, fmi2False, 0.0, 0., fmi2True, 1.);
  if (status != fmi2OK) casadi_error("fmi2SetupExperiment failed");

  // Set inputs
  for (size_t k = 0; k < vref_in_.size(); ++k) {
    // Set input k
    if (x[k]) {
      // Input is available
      fmi2Status status = dae->fmu_->set_real_(m->c, get_ptr(vref_in_[k]), vref_in_[k].size(), x[k]);
      if (status != fmi2OK) {
        casadi_warning("fmi2SetReal failed for input '" + name_in_[k] + "'");
        return 1;
      }
    } else {
      // Input is null (zero)
      static const double zero = 0;
      for (size_t i = 0; i < vref_in_[k].size(); ++i) {
        fmi2Status status = dae->fmu_->set_real_(m->c, &vref_in_[k][i], 1, &zero);
        if (status != fmi2OK) {
          casadi_warning("fmi2SetReal failed for input '" + name_in_[k] + "', component " + str(i)
            + " (value reference " + str(vref_in_[k][i]) + ")" );
          return 1;
        }
      }
    }
  }

  // Initialization mode begins
  status = dae->fmu_->enter_initialization_mode_(m->c);
  if (status != fmi2OK) casadi_error("fmi2EnterInitializationMode failed: " + str(status));

  // Initialization mode ends
  status = dae->fmu_->exit_initialization_mode_(m->c);
  if (status != fmi2OK) casadi_error("fmi2ExitInitializationMode failed");

  // Success
  return 0;
}

int FmuFunction::get_outputs(FmuFunctionMemory* m, double** r) const {
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);
  for (size_t k = 0; k < vref_out_.size(); ++k) {
    if (r[k]) {
      // If output is requested
      fmi2Status status = dae->fmu_->get_real_(m->c, get_ptr(vref_out_[k]), vref_out_[k].size(), r[k]);
      if (status != fmi2OK) {
        casadi_warning("fmi2GetReal failed for output '" + name_out_[k] + "'");
        return 1;
      }
    }
  }
  return 0;
}

int FmuFunction::eval(const double** arg, double** res, casadi_int* iw, double* w,
    void* mem) const {
  // Memory object
  auto m = static_cast<FmuFunctionMemory*>(mem);
  // Pass inputs
  if (set_inputs(m, arg)) return 1;
  // Evaluate and get outputs
  if (get_outputs(m, res)) return 1;
  return 0;
}

int FmuFunction::eval_jac(const double** arg, double** res, casadi_int* iw, double* w,
    void* mem) const {
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);
  // Dimensions
  casadi_int n_xd = nnz_in(0);
  casadi_int n_yd = nnz_out(0);
  // Outputs
  double* jac = res[0];
  // Forward seed, sensitivity
  double* fwd_xd = w; w += n_xd;
  double* fwd_yd = w; w += n_yd;
  // FMI return flag
  fmi2Status status;
  // Memory object
  auto m = static_cast<FmuFunctionMemory*>(mem);
  // Pass inputs
  if (set_inputs(m, arg)) return 1;
  // Clear seeds
  casadi_clear(fwd_xd, n_xd);
  // Calculate Jacobian, one column at a time
  for (casadi_int i = 0; i < n_xd; ++i) {
    // Set seed for column i
    fwd_xd[i] = 1.;
    // Calculate directional derivative
    status = dae->fmu_->get_directional_derivative_(m->c, get_ptr(vref_out_[0]), vref_out_[0].size(),
      get_ptr(vref_in_[0]), vref_in_[0].size(), fwd_xd, fwd_yd);
    if (status != fmi2OK) {
      casadi_warning("fmi2GetDirectionalDerivative failed");
      return 1;
    }
    // Copy column to Jacobian
    casadi_copy(fwd_yd, n_yd, jac);
    jac += n_yd;
    // Remove seed
    fwd_xd[i] = 0;
  }
  // Successful return
  return 0;
}

int FmuFunction::eval_adj(const double** arg, double** res, casadi_int* iw, double* w,
    void* mem) const {
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);
  // Dimensions
  casadi_int n_xd = nnz_in(0);
  casadi_int n_yd = nnz_out(0);
  // Inputs
  // const double* xd = arg[0];
  // const double* out_yd = arg[1];  // not used
  const double* adj_yd = arg[2];
  // Outputs
  double* adj_xd = res[0];
  // Forward seed, sensitivity for calculating columns of the Jacobian
  double* fwd_xd = w; w += n_xd;
  double* fwd_yd = w; w += n_yd;
  // FMI return flag
  fmi2Status status;
  // Memory object
  auto m = static_cast<FmuFunctionMemory*>(mem);
  // Pass inputs
  if (set_inputs(m, arg)) return 1;
  // Reset results
  casadi_clear(adj_xd, n_xd);
  // Clear seeds
  casadi_clear(fwd_xd, n_xd);
  // Calculate Jacobian, one column at a time
  for (casadi_int i = 0; i < n_xd; ++i) {
    // Set seed for column i
    fwd_xd[i] = 1.;
    // Calculate directional derivative
    status = dae->fmu_->get_directional_derivative_(m->c, get_ptr(vref_out_[0]), vref_out_[0].size(),
      get_ptr(vref_in_[0]), vref_in_[0].size(), fwd_xd, fwd_yd);
    if (status != fmi2OK) {
      casadi_warning("fmi2GetDirectionalDerivative failed");
      return 1;
    }
    // Add contribution from first seed
    adj_xd[i] += casadi_dot(n_yd, fwd_yd, adj_yd);
    // Remove seed
    fwd_xd[i] = 0;
  }
  // Successful return
  return 0;
}

bool FmuFunction::has_jacobian() const {
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);
  return dae->provides_directional_derivative_;
}

Function FmuFunction::get_jacobian(const std::string& name, const std::vector<std::string>& inames,
    const std::vector<std::string>& onames, const Dict& opts) const {
  Function ret;
  ret.own(new FmuFunctionJac(name));
  // Hack: Manually enable finite differenting (pending implementation in class)
  Dict opts2 = opts;
  opts2["enable_fd"] = true;
  ret->construct(opts2);
  return ret;
}

bool FmuFunction::has_reverse(casadi_int nadj) const {
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);
  return dae->provides_directional_derivative_ && nadj == 1;
}

Function FmuFunction::get_reverse(casadi_int nadj, const std::string& name,
    const std::vector<std::string>& inames,
    const std::vector<std::string>& onames,
    const Dict& opts) const {
  casadi_assert(nadj == 1, "Not supported");
  Function ret;
  ret.own(new FmuFunctionAdj(name));
  // Hack: Manually enable finite differenting (pending implementation in class)
  Dict opts2 = opts;
  opts2["enable_fd"] = true;
  ret->construct(opts2);
  return ret;
}

FmuFunctionJac::~FmuFunctionJac() {
  clear_mem();
}

void FmuFunctionJac::init(const Dict& opts) {
  // Call the base class initializer
  FunctionInternal::init(opts);
  // Work vectors
  alloc_w(derivative_of_.nnz_in(0), true);
  alloc_w(derivative_of_.nnz_out(0), true);
}

int FmuFunctionJac::eval(const double** arg, double** res, casadi_int* iw, double* w,
    void* mem) const {
  // Redirect to non-differentiated class
  auto m = derivative_of_.get<FmuFunction>();
  return m->eval_jac(arg, res, iw, w, mem);
}

FmuFunctionAdj::~FmuFunctionAdj() {
  clear_mem();
}

void FmuFunctionAdj::init(const Dict& opts) {
  // Call the base class initializer
  FunctionInternal::init(opts);
  // Work vectors
  alloc_w(derivative_of_.nnz_in(0), true);
  alloc_w(derivative_of_.nnz_out(0), true);
}

int FmuFunctionAdj::eval(const double** arg, double** res, casadi_int* iw, double* w,
    void* mem) const {
  // Redirect to non-differentiated class
  auto m = derivative_of_.get<FmuFunction>();
  return m->eval_adj(arg, res, iw, w, mem);
}

#endif  // WITH_FMU

} // namespace casadi
