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

  // Load on first encounter
  if (dae->fmu_ == 0) dae->init_fmu();
}

Sparsity FmuFunction::get_sparsity_in(casadi_int i) {
  return Sparsity::dense(id_in_.at(i).size(), 1);
}

Sparsity FmuFunction::get_sparsity_out(casadi_int i) {
  return Sparsity::dense(id_out_.at(i).size(), 1);
}

int FmuFunction::eval(const double** arg, double** res, casadi_int* iw, double* w,
    void* mem) const {
  // DaeBuilder instance
  casadi_assert(dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(dae_->raw_);
  // Create instance
  int m = dae->fmu_->checkout();
  // Evaluate fmu
  int flag = eval_fmu(dae, m, arg, res);
  // Release memory object
  dae->fmu_->release(m);
  // Return error flag
  return flag;
}

int FmuFunction::eval_fmu(const DaeBuilderInternal* dae, int mem,
    const double** arg, double** res) const {
  // Set inputs
  for (size_t k = 0; k < id_in_.size(); ++k) {
    for (size_t i = 0; i < id_in_[k].size(); ++i) {
      if (dae->fmu_->set(mem, id_in_[k][i], arg[k] ? arg[k][i] : 0)) return 1;
    }
  }
  // Request outputs to be evaluated
  for (size_t k = 0; k < id_out_.size(); ++k) {
    if (res[k]) {
      for (size_t i = 0; i < id_out_[k].size(); ++i) {
        if (dae->fmu_->request(mem, id_out_[k][i])) return 1;
      }
    }
  }
  // Reset solver
  if (dae->fmu_->setup_experiment(mem)) return 1;
  // Initialization mode begins
  if (dae->fmu_->enter_initialization_mode(mem)) return 1;
  // Initialization mode ends
  if (dae->fmu_->exit_initialization_mode(mem)) return 1;
  // Evaluate
  if (dae->fmu_->eval(mem)) return 1;
  // Reset solver
  if (dae->fmu_->reset(mem)) return 1;
  // Get outputs
  for (size_t k = 0; k < id_out_.size(); ++k) {
    if (res[k]) {
      for (size_t i = 0; i < id_out_[k].size(); ++i) {
        if (dae->fmu_->get(mem, id_out_[k][i], &res[k][i])) return 1;
      }
    }
  }
  // Successful return
  return 0;
}

int FmuFunction::eval_jac(const DaeBuilderInternal* dae, int mem,
    const double** arg, double** res) const {
  // Set inputs
  for (size_t k = 0; k < id_in_.size(); ++k) {
    for (size_t i = 0; i < id_in_[k].size(); ++i) {
      if (dae->fmu_->set(mem, id_in_[k][i], arg[k] ? arg[k][i] : 0)) return 1;
    }
  }
  // Reset solver
  if (dae->fmu_->setup_experiment(mem)) return 1;
  // Initialization mode begins
  if (dae->fmu_->enter_initialization_mode(mem)) return 1;
  // Initialization mode ends
  if (dae->fmu_->exit_initialization_mode(mem)) return 1;
  // Evaluate
  if (dae->fmu_->eval(mem)) return 1;
  // Outputs
  double* jac = res[0];
  // Calculate Jacobian, one column at a time
  for (casadi_int i = 0; i < id_in_[0].size(); ++i) {
    // Set seed for column i
    dae->fmu_->set_seed(mem, id_in_[0][i], 1.);
    // Request sensitivities
    for (size_t id : id_out_[0])
      if (dae->fmu_->request(mem, id)) return 1;
    // Calculate derivatives
    if (dae->fmu_->eval_derivative(mem)) return 1;
    // Get sensitivities
    for (size_t id : id_out_[0])
      if (dae->fmu_->get_sens(mem, id, jac++)) return 1;
  }
  // Reset solver
  if (dae->fmu_->reset(mem)) return 1;
  // Successful return
  return 0;
}

int FmuFunction::eval_adj(const DaeBuilderInternal* dae, int mem,
    const double** arg, double** res) const {
  // Dimensions
  casadi_int n_xd = nnz_in(0);
  // Inputs
  // const double* xd = arg[0];
  // const double* out_yd = arg[1];  // not used
  const double* adj_yd = arg[2];
  // Outputs
  double* adj_xd = res[0];
  // Set inputs
  for (size_t k = 0; k < id_in_.size(); ++k) {
    for (size_t i = 0; i < id_in_[k].size(); ++i) {
      if (dae->fmu_->set(mem, id_in_[k][i], arg[k] ? arg[k][i] : 0)) return 1;
    }
  }
  // Setup experiment
  if (dae->fmu_->setup_experiment(mem)) return 1;
  // Initialization mode begins
  if (dae->fmu_->enter_initialization_mode(mem)) return 1;
  // Initialization mode ends
  if (dae->fmu_->exit_initialization_mode(mem)) return 1;
  // Evaluate
  if (dae->fmu_->eval(mem)) return 1;
  // Reset results
  casadi_clear(adj_xd, n_xd);
  // Calculate Jacobian, one column at a time
  for (casadi_int i = 0; i < n_xd; ++i) {
    // Set seed for column i
    dae->fmu_->set_seed(mem, id_in_[0][i], 1.);
    // Request sensitivities
    for (size_t id : id_out_[0])
      if (dae->fmu_->request(mem, id)) return 1;
    // Calculate derivatives
    if (dae->fmu_->eval_derivative(mem)) return 1;
    // Get sensitivities
    for (casadi_int j = 0; j < id_out_[0].size(); ++j) {
      double J_ij;
      if (dae->fmu_->get_sens(mem, id_out_[0][j], &J_ij)) return 1;
      adj_xd[i] += adj_yd[j] * J_ij;
    }
  }
  // Reset solver
  if (dae->fmu_->reset(mem)) return 1;
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
  // Free memory
  clear_mem();
}

int FmuFunctionJac::eval(const double** arg, double** res, casadi_int* iw, double* w,
    void* mem) const {
  // Non-differentiated class
  auto self = derivative_of_.get<FmuFunction>();
  // DaeBuilder instance
  casadi_assert(self->dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(self->dae_->raw_);
  // Create instance
  int m = dae->fmu_->checkout();
  // Evaluate fmu
  int flag = self->eval_jac(dae, m, arg, res);
  // Release memory object
  dae->fmu_->release(m);
  // Return error flag
  return flag;
}

FmuFunctionAdj::~FmuFunctionAdj() {
  // Free memory
  clear_mem();
}

int FmuFunctionAdj::eval(const double** arg, double** res, casadi_int* iw, double* w,
    void* mem) const {
  // Non-differentiated class
  auto self = derivative_of_.get<FmuFunction>();
  // DaeBuilder instance
  casadi_assert(self->dae_.alive(), "DaeBuilder instance has been deleted");
  auto dae = static_cast<const DaeBuilderInternal*>(self->dae_->raw_);
  // Create instance
  int m = dae->fmu_->checkout();
  // Evaluate fmu
  int flag = self->eval_adj(dae, m, arg, res);
  // Release memory object
  dae->fmu_->release(m);
  // Return error flag
  return flag;
}

#endif  // WITH_FMU

} // namespace casadi
