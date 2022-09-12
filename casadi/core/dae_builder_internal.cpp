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


#include "dae_builder_internal.hpp"

#include <cctype>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <algorithm>

#include "casadi_misc.hpp"
#include "exception.hpp"
#include "code_generator.hpp"
#include "calculus.hpp"
#include "xml_file.hpp"
#include "external.hpp"

namespace casadi {

std::string to_string(Variable::Type v) {
  switch (v) {
  case Variable::REAL: return "real";
  case Variable::INTEGER: return "integer";
  case Variable::BOOLEAN: return "boolean";
  case Variable::STRING: return "string";
  case Variable::ENUM: return "enum";
  default: break;
  }
  return "";
}

std::string to_string(Variable::Causality v) {
  switch (v) {
  case Variable::PARAMETER: return "parameter";
  case Variable::CALCULATED_PARAMETER: return "calculatedParameter";
  case Variable::INPUT: return "input";
  case Variable::OUTPUT: return "output";
  case Variable::LOCAL: return "local";
  case Variable::INDEPENDENT: return "independent";
  default: break;
  }
  return "";
}

std::string to_string(Variable::Variability v) {
  switch (v) {
  case Variable::CONSTANT: return "constant";
  case Variable::FIXED: return "fixed";
  case Variable::TUNABLE: return "tunable";
  case Variable::DISCRETE: return "discrete";
  case Variable::CONTINUOUS: return "continuous";
  default: break;
  }
  return "";
}

std::string to_string(Variable::Initial v) {
  switch (v) {
  case Variable::EXACT: return "exact";
  case Variable::APPROX: return "approx";
  case Variable::CALCULATED: return "calculated";
  case Variable::INITIAL_NA: return "initial_na";
  default: break;
  }
  return "";
}

CASADI_EXPORT std::string to_string(Variable::Attribute v) {
  switch (v) {
  case Variable::MIN: return "min";
  case Variable::MAX: return "max";
  case Variable::NOMINAL: return "nominal";
  case Variable::START: return "start";
  default: break;
  }
  return "";
}

Variable::Initial Variable::default_initial(Variable::Causality causality,
    Variable::Variability variability) {
  // According to table in FMI 2.0.2 specification, section 2.2.7
  switch (variability) {
  case CONSTANT:
    if (causality == OUTPUT || causality == LOCAL)
      return EXACT;
    break;
  case FIXED:
    // Fall-through
  case TUNABLE:
    if (causality == PARAMETER)
      return EXACT;
    else if (causality == CALCULATED_PARAMETER || causality == LOCAL)
      return CALCULATED;
    break;
  case DISCRETE:
  // Fall-through
  case CONTINUOUS:
    if (causality == OUTPUT || causality == LOCAL)
      return CALCULATED;
    break;
  default: break;
  }
  // Initial value not available
  return INITIAL_NA;
}

Variable::Variable(const std::string& name) : name(name),
    value_reference(-1), description(""),
    type(REAL), causality(LOCAL), variability(CONTINUOUS),
    unit(""), display_unit(""),
    min(-std::numeric_limits<double>::infinity()), max(std::numeric_limits<double>::infinity()),
    nominal(1.0), start(0.0), derivative(-1), antiderivative(-1), dependency(false) {
}

void Variable::disp(std::ostream &stream, bool more) const {
  stream << name;
}

DaeBuilderInternal::~DaeBuilderInternal() {
}

DaeBuilderInternal::DaeBuilderInternal(const std::string& name) : name_(name) {
  clear_cache_ = false;
}

void DaeBuilderInternal::parse_fmi(const std::string& filename) {

  // Load
  XmlFile xml_file("tinyxml");
  XmlNode document = xml_file.parse(filename);

  // Number of variables before adding new ones
  size_t n_vars_before = variables_.size();

  // **** Add model variables ****
  {
    // Get a reference to the ModelVariables node
    const XmlNode& modvars = document[0]["ModelVariables"];

    // Add variables
    for (casadi_int i = 0; i < modvars.size(); ++i) {
      // Get a reference to the variable
      const XmlNode& vnode = modvars[i];

      // Name of variable, ensure unique
      std::string name = vnode.attribute<std::string>("name");
      casadi_assert(varind_.find(name) == varind_.end(), "Duplicate variable: " + name);

      // Create new variable
      Variable var(name);
      var.v = MX::sym(name);

      // Read common attributes, cf. FMI 2.0.2 specification, 2.2.7
      var.value_reference = vnode.attribute<casadi_int>("valueReference");
      var.description = vnode.attribute<std::string>("description", "");
      var.causality = to_enum<Variable::Causality>(
        vnode.attribute<std::string>("causality", "local"));
      var.variability = to_enum<Variable::Variability>(
        vnode.attribute<std::string>("variability", "continuous"));
      std::string initial_str = vnode.attribute<std::string>("initial", "");
      if (initial_str.empty()) {
        // Default value
        var.initial = Variable::default_initial(var.causality, var.variability);
      } else {
        // Consistency check
        casadi_assert(var.causality != Variable::INPUT && var.causality != Variable::INDEPENDENT,
          "The combination causality = '" + to_string(var.causality) + "', "
          "initial = '" + initial_str + "' is not allowed per FMI 2.0 specification.");
        // Value specified
        var.initial = to_enum<Variable::Initial>(initial_str);
      }
      // Other properties
      if (vnode.has_child("Real")) {
        const XmlNode& props = vnode["Real"];
        var.unit = props.attribute<std::string>("unit", var.unit);
        var.display_unit = props.attribute<std::string>("displayUnit", var.display_unit);
        var.min = props.attribute<double>("min", -inf);
        var.max = props.attribute<double>("max", inf);
        var.nominal = props.attribute<double>("nominal", 1.);
        var.start = props.attribute<double>("start", 0.);
        var.derivative = props.attribute<casadi_int>("derivative", var.derivative);
      }
      // Add to list of variables
      add_variable(name, var);
    }
    // Handle derivatives/antiderivatives
    for (auto it = variables_.begin() + n_vars_before; it != variables_.end(); ++it) {
      if (it->derivative >= 0) {
        // Add variable offset, make index 1
        it->derivative += n_vars_before - 1;
        // Set antiderivative
        variables_.at(it->derivative).antiderivative = it - variables_.begin();
      }
    }
  }

  // **** Process model structure ****
  if (document[0].has_child("ModelStructure")) {
    // Get a reference to the ModelVariables node
    const XmlNode& modst = document[0]["ModelStructure"];
    // Test both Outputs and Derivatives
    for (const char* dtype : {"Outputs", "Derivatives"}) {
      // Derivative variables
      if (modst.has_child(dtype)) {
        const XmlNode& outputs = modst[dtype];
        for (casadi_int i = 0; i < outputs.size(); ++i) {
          // Get a reference to the output
          const XmlNode& onode = outputs[i];
          // Read attribute
          casadi_int index = onode.attribute<casadi_int>("index", -1);
          casadi_assert(index >= 1, "Non-positive output index");
          // Convert to index in variables list
          index += n_vars_before - 1;
          // Get dependencies
          std::vector<casadi_int> dependencies = onode.attribute<std::vector<casadi_int>>(
            "dependencies", {});
          // Convert to indices in variables list
          for (casadi_int& d : dependencies) {
            // Consistency check, add offset
            casadi_assert(d >= 1, "Non-positive dependency index");
            d += n_vars_before - 1;
            // Mark corresponding variable as dependency
            variables_.at(d).dependency = true;
          }
        }
      }
    }
  }

  // **** Postprocess / sort variables ****
  for (auto it = variables_.begin() + n_vars_before; it != variables_.end(); ++it) {
    // Sort by types
    if (it->causality == Variable::INDEPENDENT) {
      // Independent (time) variable
      t_.push_back(it->v);
    } else if (it->causality == Variable::INPUT) {
      u_.push_back(it->v);
    } else if (it->variability == Variable::CONSTANT) {
      // Named constant
      c_.push_back(it->v);
      it->beq = it->start;
    } else if (it->variability == Variable::FIXED || it->variability == Variable::TUNABLE) {
      p_.push_back(it->v);
    } else if (it->variability == Variable::CONTINUOUS) {
      if (it->antiderivative >= 0) {
        // Is the variable needed to calculate other states, algebraic variables?
        if (it->dependency) {
          // Add to list of differential equations
          x_.push_back(it->v);
          ode_.push_back(variables_.at(it->antiderivative).v);
        } else {
          // Add to list of quadrature equations
          q_.push_back(it->v);
          quad_.push_back(variables_.at(it->antiderivative).v);
        }
      } else if (it->dependency || it->derivative >= 0) {
        // Add to list of algebraic equations
        z_.push_back(it->v);
        alg_.push_back(it->v - nan);
      }
      // Is it (also) an output variable?
      if (it->causality == Variable::OUTPUT) {
        y_.push_back(it->v);
        it->beq = it->v;
      }
    } else if (it->dependency) {
      casadi_warning("Cannot sort " + it->name);
    }
  }
}

Variable& DaeBuilderInternal::read_variable(const XmlNode& node) {
  // Qualified name
  std::string qn = qualified_name(node);

  // Find and return the variable
  return variable(qn);
}

MX DaeBuilderInternal::read_expr(const XmlNode& node) {
  const std::string& fullname = node.name();
  if (fullname.find("exp:")== std::string::npos) {
    casadi_error("DaeBuilderInternal::read_expr: unknown - expression is supposed to "
                 "start with 'exp:' , got " + fullname);
  }

  // Chop the 'exp:'
  std::string name = fullname.substr(4);

  // The switch below is alphabetical, and can be thus made more efficient,
  // for example by using a switch statement of the first three letters,
  // if it would ever become a bottleneck
  if (name=="Add") {
    return read_expr(node[0]) + read_expr(node[1]);
  } else if (name=="Acos") {
    return acos(read_expr(node[0]));
  } else if (name=="Asin") {
    return asin(read_expr(node[0]));
  } else if (name=="Atan") {
    return atan(read_expr(node[0]));
  } else if (name=="Cos") {
    return cos(read_expr(node[0]));
  } else if (name=="Der") {
    return variables_.at(read_variable(node[0]).derivative).v;
  } else if (name=="Div") {
    return read_expr(node[0]) / read_expr(node[1]);
  } else if (name=="Exp") {
    return exp(read_expr(node[0]));
  } else if (name=="Identifier") {
    return read_variable(node).v;
  } else if (name=="IntegerLiteral") {
    casadi_int val;
    node.getText(val);
    return val;
  } else if (name=="Instant") {
    double val;
    node.getText(val);
    return val;
  } else if (name=="Log") {
    return log(read_expr(node[0]));
  } else if (name=="LogLeq") { // Logical less than equal
    return read_expr(node[0]) <= read_expr(node[1]);
  } else if (name=="LogGeq") { // Logical greater than equal
    return read_expr(node[0]) >= read_expr(node[1]);
  } else if (name=="LogLt") { // Logical less than
    return read_expr(node[0]) < read_expr(node[1]);
  } else if (name=="LogGt") { // Logical greater than
    return read_expr(node[0]) > read_expr(node[1]);
  } else if (name=="Max") {
    return fmax(read_expr(node[0]), read_expr(node[1]));
  } else if (name=="Min") {
    return fmin(read_expr(node[0]), read_expr(node[1]));
  } else if (name=="Mul") { // Multiplication
    return read_expr(node[0]) * read_expr(node[1]);
  } else if (name=="Neg") {
    return -read_expr(node[0]);
  } else if (name=="NoEvent") {
    // NOTE: This is a workaround, we assume that whenever NoEvent occurs,
    // what is meant is a switch
    casadi_int n = node.size();

    // Default-expression
    MX ex = read_expr(node[n-1]);

    // Evaluate ifs
    for (casadi_int i=n-3; i>=0; i -= 2) {
      ex = if_else(read_expr(node[i]), read_expr(node[i+1]), ex);
    }

    return ex;
  } else if (name=="Pow") {
    return pow(read_expr(node[0]), read_expr(node[1]));
  } else if (name=="RealLiteral") {
    double val;
    node.getText(val);
    return val;
  } else if (name=="Sin") {
    return sin(read_expr(node[0]));
  } else if (name=="Sqrt") {
    return sqrt(read_expr(node[0]));
  } else if (name=="StringLiteral") {
    throw CasadiException(node.getText());
  } else if (name=="Sub") {
    return read_expr(node[0]) - read_expr(node[1]);
  } else if (name=="Tan") {
    return tan(read_expr(node[0]));
  } else if (name=="Time") {
    return t_.at(0);
  } else if (name=="TimedVariable") {
    return read_variable(node[0]).v;
  }

  // throw error if reached this point
  throw CasadiException(std::string("DaeBuilderInternal::read_expr: Unknown node: ") + name);

}

void DaeBuilderInternal::disp(std::ostream& stream, bool more) const {
  // Assert correctness
  if (more) sanity_check();

  // Print dimensions
  stream << "nx = " << x_.size() << ", "
         << "nz = " << z_.size() << ", "
         << "nq = " << q_.size() << ", "
         << "ny = " << y_.size() << ", "
         << "np = " << p_.size() << ", "
         << "nc = " << c_.size() << ", "
         << "nd = " << d_.size() << ", "
         << "nw = " << w_.size() << ", "
         << "nu = " << u_.size();

  // Quick return?
  if (!more) return;
  stream << std::endl;

  // Print the functions
  if (!fun_.empty()) {
    stream << "Functions" << std::endl;
    for (const Function& f : fun_) {
      stream << "  " << f << std::endl;
    }
  }

  // Print the variables
  stream << "Variables" << std::endl;
  if (!t_.empty()) stream << "  t = " << str(t_.at(0)) << std::endl;
  if (!c_.empty()) stream << "  c = " << str(c_) << std::endl;
  if (!p_.empty()) stream << "  p = " << str(p_) << std::endl;
  if (!d_.empty()) stream << "  d = " << str(d_) << std::endl;
  if (!x_.empty()) stream << "  x = " << str(x_) << std::endl;
  if (!z_.empty()) stream << "  z = " << str(z_) << std::endl;
  if (!q_.empty()) stream << "  q = " << str(q_) << std::endl;
  if (!y_.empty()) stream << "  y = " << str(y_) << std::endl;
  if (!w_.empty()) stream << "  w = " << str(w_) << std::endl;
  if (!u_.empty()) stream << "  u = " << str(u_) << std::endl;

  if (!c_.empty()) {
    stream << "Constants" << std::endl;
    for (const MX& c : c_) {
      stream << "  " << str(c) << " == " << str(variable(c.name()).beq) << std::endl;
    }
  }

  if (!d_.empty()) {
    stream << "Dependent parameters" << std::endl;
    for (const MX& d : d_) {
      stream << "  " << str(d) << " == " << str(variable(d.name()).beq) << std::endl;
    }
  }

  if (!w_.empty()) {
    stream << "Dependent variables" << std::endl;
    for (const MX& w : w_) {
      stream << "  " << str(w) << " == " << str(variable(w.name()).beq) << std::endl;
    }
  }

  if (!x_.empty()) {
    stream << "Differential equations" << std::endl;
    for (casadi_int k=0; k<x_.size(); ++k) {
      stream << "  der(" << str(x_[k]) << ") == " << str(ode_[k]) << std::endl;
    }
  }

  if (!alg_.empty()) {
    stream << "Algebraic equations" << std::endl;
    for (casadi_int k=0; k<z_.size(); ++k) {
      stream << "  0 == " << str(alg_[k]) << std::endl;
    }
  }

  if (!q_.empty()) {
    stream << "Quadrature equations" << std::endl;
    for (casadi_int k=0; k<q_.size(); ++k) {
      stream << "  " << str(der(q_[k])) << " == " << str(quad_[k]) << std::endl;
    }
  }

  if (!init_lhs_.empty()) {
    stream << "Initial equations" << std::endl;
    for (casadi_int k=0; k<init_lhs_.size(); ++k) {
      stream << "  " << str(init_lhs_.at(k)) << " == " << str(init_rhs_.at(k))
        << std::endl;
    }
  }

  if (!y_.empty()) {
    stream << "Output variables" << std::endl;
    for (const MX& y : y_) {
      stream << "  " << str(y) << " == " << str(variable(y.name()).beq) << std::endl;
    }
  }
}

void DaeBuilderInternal::eliminate_quad() {
  // Move all the quadratures to the list of differential states
  x_.insert(x_.end(), q_.begin(), q_.end());
  q_.clear();
}

void DaeBuilderInternal::sort_d() {
  std::vector<MX> ddef = this->ddef();
  sort_dependent(d_, ddef);
}

void DaeBuilderInternal::sort_w() {
  std::vector<MX> wdef = this->wdef();
  sort_dependent(w_, wdef);
}

void DaeBuilderInternal::sort_z(const std::vector<std::string>& z_order) {
  // Make sure lengths agree
  casadi_assert(z_order.size() == z_.size(), "Dimension mismatch");
  // Mark existing components in z
  std::vector<bool> old_z(variables_.size(), false);
  for (size_t i = 0; i < z_.size(); ++i) {
    std::string s = z_.at(i).name();
    auto it = varind_.find(s);
    casadi_assert(it != varind_.end(), "No such variable: \"" + s + "\".");
    old_z.at(it->second) = true;
  }
  // New vector of z
  std::vector<MX> new_z;
  new_z.reserve(z_order.size());
  for (const std::string& s : z_order) {
    auto it = varind_.find(s);
    casadi_assert(it != varind_.end(), "No such variable: \"" + s + "\".");
    casadi_assert(old_z.at(it->second), "Variable \"" + s + "\" is not an algebraic variable.");
    new_z.push_back(variables_.at(it->second).v);
  }
  // Success: Update z
  std::copy(new_z.begin(), new_z.end(), z_.begin());
}

void DaeBuilderInternal::clear_in(const std::string& v) {
  switch (to_enum<DaeBuilderInternalIn>(v)) {
  case DAE_BUILDER_T: return t_.clear();
  case DAE_BUILDER_P: return p_.clear();
  case DAE_BUILDER_U: return u_.clear();
  case DAE_BUILDER_X: return x_.clear();
  case DAE_BUILDER_Z: return z_.clear();
  case DAE_BUILDER_Q: return q_.clear();
  case DAE_BUILDER_C: return c_.clear();
  case DAE_BUILDER_D: return d_.clear();
  case DAE_BUILDER_W: return w_.clear();
  case DAE_BUILDER_Y: return y_.clear();
  }
  casadi_error("Cannot clear input: " + v);
}

void DaeBuilderInternal::clear_out(const std::string& v) {
  switch (to_enum<DaeBuilderInternalOut>(v)) {
  case DAE_BUILDER_ODE: return ode_.clear();
  case DAE_BUILDER_ALG: return alg_.clear();
  case DAE_BUILDER_QUAD: return quad_.clear();
  }
  casadi_error("Cannot clear output: " + v);
}

void DaeBuilderInternal::prune(bool prune_p, bool prune_u) {
  // Function inputs and outputs
  std::vector<MX> f_in, f_out, v;
  std::vector<std::string> f_in_name, f_out_name;
  // Collect all DAE input variables with at least one entry, skip u
  for (casadi_int i = 0; i != DAE_BUILDER_NUM_IN; ++i) {
    if (prune_p && i == DAE_BUILDER_P) continue;
    if (prune_u && i == DAE_BUILDER_U) continue;
    v = input(static_cast<DaeBuilderInternalIn>(i));
    if (!v.empty()) {
      f_in.push_back(vertcat(v));
      f_in_name.push_back(to_string(static_cast<DaeBuilderInternalIn>(i)));
    }
  }
  // Collect all DAE output variables with at least one entry
  for (casadi_int i = 0; i != DAE_BUILDER_NUM_OUT; ++i) {
    v = output(static_cast<DaeBuilderInternalOut>(i));
    if (!v.empty()) {
      f_out.push_back(vertcat(v));
      f_out_name.push_back(to_string(static_cast<DaeBuilderInternalOut>(i)));
    }
  }
  // Create a function
  Function f("prune_fcn", f_in, f_out, f_in_name, f_out_name);
  // Mark which variables are free
  std::vector<bool> free_variables(variables_.size(), false);
  for (const std::string& s : f.get_free()) {
    auto it = varind_.find(s);
    casadi_assert(it != varind_.end(), "No such variable: \"" + s + "\".");
    free_variables.at(it->second) = true;
  }
  // Prune p
  if (prune_p) {
    size_t np = 0;
    for (size_t i = 0; i < p_.size(); ++i) {
      std::string s = p_.at(i).name();
      auto it = varind_.find(s);
      casadi_assert(it != varind_.end(), "No such variable: \"" + s + "\".");
      if (!free_variables.at(it->second)) p_.at(np++) = p_.at(i);
    }
    p_.resize(np);
  }
  // Prune u
  if (prune_u) {
    size_t nu = 0;
    for (size_t i = 0; i < u_.size(); ++i) {
      std::string s = u_.at(i).name();
      auto it = varind_.find(s);
      casadi_assert(it != varind_.end(), "No such variable: \"" + s + "\".");
      if (!free_variables.at(it->second)) u_.at(nu++) = u_.at(i);
    }
    u_.resize(nu);
  }
}

const Variable& DaeBuilderInternal::variable(const std::string& name) const {
  return const_cast<DaeBuilderInternal*>(this)->variable(name);
}

Variable& DaeBuilderInternal::variable(const std::string& name) {
  // Find the variable
  auto it = varind_.find(name);
  if (it == varind_.end()) casadi_error("No such variable: \"" + name + "\".");

  // Return the variable
  return variables_.at(it->second);
}

bool DaeBuilderInternal::has_variable(const std::string& name) const {
  return varind_.find(name) != varind_.end();
}

void DaeBuilderInternal::add_variable(const std::string& name, const Variable& var) {
  // Try to find the component
  casadi_assert(!has_variable(name), "Variable \"" + name + "\" has already been added.");
  // Add to the map of all variables
  varind_[name] = variables_.size();
  variables_.push_back(var);
  // Clear cache
  clear_cache_ = true;
}

void DaeBuilderInternal::sanity_check() const {
  // Time
  if (!t_.empty()) {
    casadi_assert(t_.size() == 1, "At most one time variable allowed");
    casadi_assert(t_[0].is_symbolic(), "Non-symbolic time t");
    casadi_assert(t_[0].is_scalar(), "Non-scalar time t");
  }

  // Differential states
  casadi_assert(x_.size()==ode_.size(),
                        "x and ode have different lengths");
  for (casadi_int i=0; i<x_.size(); ++i) {
    casadi_assert(x_[i].size()==ode_[i].size(),
                          "ode has wrong dimensions");
    casadi_assert(x_[i].is_symbolic(), "Non-symbolic state x");
  }

  // Algebraic variables/equations
  casadi_assert(z_.size()==alg_.size(),
                        "z and alg have different lengths");
  for (casadi_int i=0; i<z_.size(); ++i) {
    casadi_assert(z_[i].is_symbolic(), "Non-symbolic algebraic variable z");
    casadi_assert(z_[i].size()==alg_[i].size(),
                          "alg has wrong dimensions");
  }

  // Quadrature states/equations
  casadi_assert(q_.size()==quad_.size(), "q and quad have different lengths");
  for (casadi_int i=0; i<q_.size(); ++i) {
    casadi_assert(q_[i].is_symbolic(), "Non-symbolic quadrature state q");
    casadi_assert(q_[i].size()==quad_[i].size(),
                          "quad has wrong dimensions");
  }

  // Dependent parameters
  for (casadi_int i=0; i<d_.size(); ++i) {
    casadi_assert(d_[i].is_symbolic(), "Non-symbolic dependent parameter d");
  }

  // Dependent variables
  for (casadi_int i=0; i<w_.size(); ++i) {
    casadi_assert(w_[i].is_symbolic(), "Non-symbolic dependent parameter v");
  }

  // Output equations
  for (casadi_int i=0; i<y_.size(); ++i) {
    casadi_assert(y_[i].is_symbolic(), "Non-symbolic output y");
  }

  // Control
  for (casadi_int i=0; i<u_.size(); ++i) {
    casadi_assert(u_[i].is_symbolic(), "Non-symbolic control u");
  }

  // Parameter
  for (casadi_int i=0; i<p_.size(); ++i) {
    casadi_assert(p_[i].is_symbolic(), "Non-symbolic parameter p");
  }

  // Initial equations
  casadi_assert(init_lhs_.size() == init_rhs_.size(),
    "init_lhs and init_rhs have different lengths");
}

std::string DaeBuilderInternal::qualified_name(const XmlNode& nn) {
  // Stringstream to assemble name
  std::stringstream qn;

  for (casadi_int i=0; i<nn.size(); ++i) {
    // Add a dot
    if (i!=0) qn << ".";

    // Get the name part
    qn << nn[i].attribute<std::string>("name");

    // Get the index, if any
    if (nn[i].size()>0) {
      casadi_int ind;
      nn[i]["exp:ArraySubscripts"]["exp:IndexExpression"]["exp:IntegerLiteral"].getText(ind);
      qn << "[" << ind << "]";
    }
  }

  // Return the name
  return qn.str();
}

MX DaeBuilderInternal::var(const std::string& name) const {
  return variable(name).v;
}

MX DaeBuilderInternal::der(const std::string& name) const {
  return variables_.at(variable(name).derivative).v;
}

MX DaeBuilderInternal::der(const MX& var) const {
  casadi_assert_dev(var.is_column() && var.is_symbolic());
  return der(var.name());
}

void DaeBuilderInternal::eliminate_w() {
  // Quick return if no w
  if (w_.empty()) return;
  // Ensure variables are sorted
  sort_w();
  // Expressions where the variables are also being used
  std::vector<MX> ex;
  ex.insert(ex.end(), alg_.begin(), alg_.end());
  ex.insert(ex.end(), ode_.begin(), ode_.end());
  ex.insert(ex.end(), quad_.begin(), quad_.end());
  // Also include certain attributes in variables
  for (const Variable& v : variables_) {
    if (!v.min.is_constant()) ex.push_back(v.min);
    if (!v.max.is_constant()) ex.push_back(v.max);
    if (!v.nominal.is_constant()) ex.push_back(v.nominal);
    if (!v.start.is_constant()) ex.push_back(v.start);
    if (!v.beq.is_constant()) ex.push_back(v.beq);
  }
  // Perform elimination
  std::vector<MX> wdef = this->wdef();
  substitute_inplace(w_, wdef, ex);
  // Clear list of dependent variables
  w_.clear();
  // Get algebraic equations
  auto it = ex.begin();
  std::copy(it, it + alg_.size(), alg_.begin());
  it += alg_.size();
  // Get differential equations
  std::copy(it, it + ode_.size(), ode_.begin());
  it += ode_.size();
  // Get quadrature equations
  std::copy(it, it + quad_.size(), quad_.begin());
  it += quad_.size();
  // Get variable attributes
  for (Variable& v : variables_) {
    if (!v.min.is_constant()) v.min = *it++;
    if (!v.max.is_constant()) v.max = *it++;
    if (!v.nominal.is_constant()) v.nominal = *it++;
    if (!v.start.is_constant()) v.start = *it++;
    if (!v.beq.is_constant()) v.beq = *it++;
  }
  // Consistency check
  casadi_assert_dev(it == ex.end());
}

void DaeBuilderInternal::lift(bool lift_shared, bool lift_calls) {
  // Not tested if w is non-empty before
  if (!w_.empty()) casadi_warning("'w' already has entries");
  // Expressions where the variables are also being used
  std::vector<MX> ex;
  ex.insert(ex.end(), alg_.begin(), alg_.end());
  ex.insert(ex.end(), ode_.begin(), ode_.end());
  ex.insert(ex.end(), quad_.begin(), quad_.end());
  for (const MX& y : y_) ex.push_back(variable(y.name()).beq);
  // Lift expressions
  std::vector<MX> new_w, new_wdef;
  Dict opts{{"lift_shared", lift_shared}, {"lift_calls", lift_calls},
    {"prefix", "w_"}, {"suffix", ""}, {"offset", static_cast<casadi_int>(w_.size())}};
  extract(ex, new_w, new_wdef, opts);
  // Register as dependent variables
  for (size_t i = 0; i < new_w.size(); ++i) {
    Variable v(new_w.at(i).name());
    v.v = new_w.at(i);
    v.beq = new_wdef.at(i);
    add_variable(v.name, v);
    w_.push_back(v.v);
  }
  // Get algebraic equations
  auto it = ex.begin();
  std::copy(it, it + alg_.size(), alg_.begin());
  it += alg_.size();
  // Get differential equations
  std::copy(it, it + ode_.size(), ode_.begin());
  it += ode_.size();
  // Get quadrature equations
  std::copy(it, it + quad_.size(), quad_.begin());
  it += quad_.size();
  // Get output equations
  for (const MX& y : y_) variable(y.name()).beq = *it++;
  // Consistency check
  casadi_assert_dev(it == ex.end());
}

std::string to_string(DaeBuilderInternal::DaeBuilderInternalIn v) {
  switch (v) {
  case DaeBuilderInternal::DAE_BUILDER_T: return "t";
  case DaeBuilderInternal::DAE_BUILDER_P: return "p";
  case DaeBuilderInternal::DAE_BUILDER_U: return "u";
  case DaeBuilderInternal::DAE_BUILDER_X: return "x";
  case DaeBuilderInternal::DAE_BUILDER_Z: return "z";
  case DaeBuilderInternal::DAE_BUILDER_Q: return "q";
  case DaeBuilderInternal::DAE_BUILDER_C: return "c";
  case DaeBuilderInternal::DAE_BUILDER_D: return "d";
  case DaeBuilderInternal::DAE_BUILDER_W: return "w";
  case DaeBuilderInternal::DAE_BUILDER_Y: return "y";
  default: break;
  }
  return "";
}

std::string to_string(DaeBuilderInternal::DaeBuilderInternalOut v) {
  switch (v) {
  case DaeBuilderInternal::DAE_BUILDER_ODE: return "ode";
  case DaeBuilderInternal::DAE_BUILDER_ALG: return "alg";
  case DaeBuilderInternal::DAE_BUILDER_QUAD: return "quad";
  case DaeBuilderInternal::DAE_BUILDER_DDEF: return "ddef";
  case DaeBuilderInternal::DAE_BUILDER_WDEF: return "wdef";
  case DaeBuilderInternal::DAE_BUILDER_YDEF: return "ydef";
  default: break;
  }
  return "";
}

const std::vector<MX>& DaeBuilderInternal::input(DaeBuilderInternalIn ind) const {
  switch (ind) {
  case DAE_BUILDER_T: return t_;
  case DAE_BUILDER_C: return c_;
  case DAE_BUILDER_P: return p_;
  case DAE_BUILDER_D: return d_;
  case DAE_BUILDER_W: return w_;
  case DAE_BUILDER_U: return u_;
  case DAE_BUILDER_X: return x_;
  case DAE_BUILDER_Z: return z_;
  case DAE_BUILDER_Q: return q_;
  case DAE_BUILDER_Y: return y_;
  default:
    {
      static std::vector<MX> dummy;
      return dummy;
    }
  }
}

std::vector<MX> DaeBuilderInternal::input(const std::vector<DaeBuilderInternalIn>& ind) const {
  std::vector<MX> ret(ind.size());
  for (casadi_int i=0; i<ind.size(); ++i) {
    ret[i] = vertcat(input(ind[i]));
  }
  return ret;
}

std::vector<MX> DaeBuilderInternal::output(DaeBuilderInternalOut ind) const {
  switch (ind) {
  case DAE_BUILDER_ODE: return ode_;
  case DAE_BUILDER_ALG: return alg_;
  case DAE_BUILDER_QUAD: return quad_;
  case DAE_BUILDER_DDEF: return ddef();
  case DAE_BUILDER_WDEF: return wdef();
  case DAE_BUILDER_YDEF: return ydef();
  default: return std::vector<MX>();
  }
}

std::vector<MX> DaeBuilderInternal::output(const std::vector<DaeBuilderInternalOut>& ind) const {
  std::vector<MX> ret(ind.size());
  for (casadi_int i=0; i<ind.size(); ++i) {
    ret[i] = vertcat(output(ind[i]));
  }
  return ret;
}

void DaeBuilderInternal::add_lc(const std::string& name,
                      const std::vector<std::string>& f_out) {
  // Make sure object valid
  sanity_check();

  // Make sure name is valid
  casadi_assert(!name.empty(), "DaeBuilderInternal::add_lc: \"name\" is empty");
  for (std::string::const_iterator i=name.begin(); i!=name.end(); ++i) {
    casadi_assert(isalnum(*i),
                          "DaeBuilderInternal::add_lc: \"name\" must be alphanumeric");
  }

  // Consistency checks
  casadi_assert(!f_out.empty(), "DaeBuilderInternal::add_lc: Linear combination is empty");
  std::vector<bool> in_use(DAE_BUILDER_NUM_OUT, false);
  for (casadi_int i=0; i<f_out.size(); ++i) {
    DaeBuilderInternalOut oind = to_enum<DaeBuilderInternalOut>(f_out[i]);
    casadi_assert(!in_use[oind], "DaeBuilderInternal::add_lc: Duplicate expression " + f_out[i]);
    in_use[oind] = true;
  }

  std::vector<std::string>& ret1 = lc_[name];
  if (!ret1.empty()) casadi_warning("DaeBuilderInternal::add_lc: Overwriting " << name);
  ret1 = f_out;
}

Function DaeBuilderInternal::create(const std::string& fname,
    const std::vector<std::string>& s_in,
    const std::vector<std::string>& s_out, bool sx, bool lifted_calls) const {
  // Are there any '_' in the names?
  bool with_underscore = false;
  for (auto s_io : {&s_in, &s_out}) {
    for (const std::string& s : *s_io) {
      with_underscore = with_underscore || std::count(s.begin(), s.end(), '_');
    }
  }
  // Replace '_' with ':', if needed
  if (with_underscore) {
    std::vector<std::string> s_in_mod(s_in), s_out_mod(s_out);
    for (auto s_io : {&s_in_mod, &s_out_mod}) {
      for (std::string& s : *s_io) replace(s.begin(), s.end(), '_', ':');
    }
    // Recursive call
    return create(fname, s_in_mod, s_out_mod, sx, lifted_calls);
  }
  // Check if dependent variables are given and needed
  bool elim_w = false;
  if (!w_.empty()) {
    // Dependent variables exists, eliminate unless v is given
    elim_w = true;
    for (const std::string& s : s_in) {
      if (s == "w") {
        // Dependent variables are given
        elim_w = false;
        break;
      }
    }
  }
  // Are lifted calls really needed?
  if (lifted_calls) {
    // Consistency check
    casadi_assert(!elim_w, "Lifted calls cannot be used if dependent variables are eliminated");
    // Only lift calls if really needed
    lifted_calls = false;
    for (const MX& vdef_comp : wdef()) {
      if (vdef_comp.is_output()) {
        // There are indeed function calls present
        lifted_calls = true;
        break;
      }
    }
  }
  // Call factory without lifted calls
  std::string fname_nocalls = lifted_calls ? fname + "_nocalls" : fname;
  Function ret = oracle(sx, elim_w, lifted_calls).factory(fname_nocalls, s_in, s_out, lc_);
  // If no lifted calls, done
  if (!lifted_calls) return ret;
  // MX expressions for ret without lifted calls
  std::vector<MX> ret_in = ret.mx_in();
  std::vector<MX> ret_out = ret(ret_in);
  // Offsets in v
  std::vector<casadi_int> h_offsets = offset(w_);
  // Split "w", "lam_wdef" into components
  std::vector<MX> v_in, lam_vdef_in;
  for (size_t i = 0; i < s_in.size(); ++i) {
    if (ret.name_in(i) == "w") {
      v_in = vertsplit(ret_in[i], h_offsets);
    } else if (ret.name_in(i) == "lam_wdef") {
      lam_vdef_in = vertsplit(ret_in[i], h_offsets);
    }
  }
  // Map dependent variables into index in vector
  std::map<MXNode*, size_t> v_map;
  for (size_t i = 0; i < w_.size(); ++i) {
    v_map[w_.at(i).get()] = i;
  }
  // Definitions of w
  std::vector<MX> wdef = this->wdef();
  // Collect all the call nodes
  std::map<MXNode*, CallIO> call_nodes;
  for (size_t vdefind = 0; vdefind < wdef.size(); ++vdefind) {
    // Current element handled
    const MX& vdefref = wdef.at(vdefind);
    // Handle function call nodes
    if (vdefref.is_output()) {
      // Get function call node
      MX c = vdefref.dep(0);
      // Find the corresponding call node in the map
      auto call_it = call_nodes.find(c.get());
      // If first time this call node is encountered
      if (call_it == call_nodes.end()) {
        // Create new CallIO struct
        CallIO cio;
        // Save function instance
        cio.f = c.which_function();
        // Expressions for function call inputs
        cio.v.resize(c.n_dep(), -1);
        cio.arg.resize(cio.v.size());
        for (casadi_int i = 0; i < cio.v.size(); ++i) {
          if (c.dep(i).is_constant()) {
            cio.arg.at(i) = c.dep(i);
          } else {
            size_t v_ind = v_map.at(c.dep(i).get());
            cio.v.at(i) = v_ind;
            cio.arg.at(i) = v_in.at(v_ind);
          }
        }
        // Allocate memory for function call outputs
        cio.vdef.resize(c.n_out(), -1);
        cio.res.resize(cio.vdef.size());
        // Allocate memory for adjoint seeds, if any
        if (!lam_vdef_in.empty()) cio.adj1_arg.resize(c.n_out());
        // Save to map and update iterator
        call_it = call_nodes.insert(std::make_pair(c.get(), cio)).first;
      }
      // Which output of the function are we calculating?
      casadi_int oind = vdefref.which_output();
      // Save output expression to structure
      call_it->second.vdef.at(oind) = vdefind;
      call_it->second.res.at(oind) = v_in.at(vdefind);
      // Save adjoint seed to structure, if any
      if (!lam_vdef_in.empty()) call_it->second.adj1_arg.at(oind) = lam_vdef_in.at(vdefind);
    }
  }
  // Additional term in jac_vdef_v
  for (size_t i = 0; i < ret_out.size(); ++i) {
    if (ret.name_out(i) == "jac_wdef_w") {
      ret_out.at(i) += jac_vdef_v_from_calls(call_nodes, h_offsets);
    }
  }
  // Additional term in hess_?_v_v where ? is any linear combination containing vdef
  MX extra_hess_v_v;  // same for all linear combinations, if multiple
  for (auto&& e : lc_) {
    // Find out of vdef is part of the linear combination
    bool has_vdef = false;
    for (const std::string& r : e.second) {
      if (r == "wdef") {
        has_vdef = true;
        break;
      }
    }
    // Skip if linear combination does not depend on vdef
    if (!has_vdef) continue;
    // Search for matching function outputs
    for (size_t i = 0; i < ret_out.size(); ++i) {
      if (ret.name_out(i) == "hess_" + e.first + "_w_w") {
        // Calculate contribution to hess_?_v_v
        if (extra_hess_v_v.is_empty())
          extra_hess_v_v = hess_v_v_from_calls(call_nodes, h_offsets);
        // Add contribution to output
        ret_out.at(i) += extra_hess_v_v;
      }
    }
  }
  // Assemble modified return function and return
  ret = Function(fname, ret_in, ret_out, ret.name_in(), ret.name_out());
  return ret;
}

MX DaeBuilderInternal::jac_vdef_v_from_calls(std::map<MXNode*, CallIO>& call_nodes,
    const std::vector<casadi_int>& h_offsets) const {
  // Calculate all Jacobian expressions
  for (auto call_it = call_nodes.begin(); call_it != call_nodes.end(); ++call_it) {
    call_it->second.calc_jac();
  }
  // Row offsets in jac_vdef_v
  casadi_int voffset_begin = 0, voffset_end = 0, voffset_last = 0;
  // Vertical and horizontal slices of jac_vdef_v
  std::vector<MX> vblocks, hblocks;
  // All blocks for this block row
  std::map<size_t, MX> jac_brow;
  // Definitions of w
  std::vector<MX> wdef = this->wdef();
  // Collect all Jacobian blocks
  for (size_t vdefind = 0; vdefind < wdef.size(); ++vdefind) {
    // Current element handled
    const MX& vdefref = wdef.at(vdefind);
    // Update vertical offset
    voffset_begin = voffset_end;
    voffset_end += vdefref.numel();
    // Handle function call nodes
    if (vdefref.is_output()) {
      // Which output of the function are we calculating?
      casadi_int oind = vdefref.which_output();
      // Get function call node
      MX c = vdefref.dep(0);
      // Find data about inputs and outputs
      auto call_it = call_nodes.find(c.get());
      casadi_assert_dev(call_it != call_nodes.end());
      // Collect all blocks for this block row
      jac_brow.clear();
      for (casadi_int iind = 0; iind < call_it->second.arg.size(); ++iind) {
        size_t vind = call_it->second.v.at(iind);
        if (vind != size_t(-1)) {
          jac_brow[vind] = call_it->second.jac(oind, iind);
        }
      }
      // Add empty rows to vblocks, if any
      if (voffset_last != voffset_begin) {
        vblocks.push_back(MX(voffset_begin - voffset_last, h_offsets.back()));
      }
      // Collect horizontal blocks
      hblocks.clear();
      casadi_int hoffset = 0;
      for (auto e : jac_brow) {
        // Add empty block before Jacobian block, if needed
        if (hoffset < h_offsets.at(e.first))
          hblocks.push_back(MX(vdefref.numel(), h_offsets.at(e.first) - hoffset));
        // Add Jacobian block
        hblocks.push_back(e.second);
        // Update offsets
        hoffset = h_offsets.at(e.first + 1);
      }
      // Add trailing empty block, if needed
      if (hoffset < h_offsets.back())
        hblocks.push_back(MX(vdefref.numel(), h_offsets.back() - hoffset));
      // Add new block row to vblocks
      vblocks.push_back(horzcat(hblocks));
      // Keep track of the offset handled in jac_brow
      voffset_last = voffset_end;
    }
  }
  // Add empty trailing row to vblocks, if any
  if (voffset_last != voffset_end) {
    vblocks.push_back(MX(voffset_end - voffset_last, h_offsets.back()));
  }
  // Return additional term in jac_vdef_v
  return vertcat(vblocks);
}

MX DaeBuilderInternal::hess_v_v_from_calls(std::map<MXNode*, CallIO>& call_nodes,
    const std::vector<casadi_int>& h_offsets) const {
  // Calculate all Hessian expressions
  for (auto&& call_ref : call_nodes) call_ref.second.calc_hess();
  // Row offsets in hess_v_v
  casadi_int voffset_begin = 0, voffset_end = 0, voffset_last = 0;
  // Vertical and horizontal slices of hess_v_v
  std::vector<MX> vblocks, hblocks;
  // All blocks for a block row
  std::map<size_t, MX> hess_brow;
  // Loop over block rows
  for (size_t vind1 = 0; vind1 < w_.size(); ++vind1) {
    // Current element handled
    const MX& vref = w_.at(vind1);
    // Update vertical offset
    voffset_begin = voffset_end;
    voffset_end += vref.numel();
    // Collect all blocks for this block row
    hess_brow.clear();
    for (auto&& call_ref : call_nodes) {
      // Locate the specific index
      for (size_t iind1 = 0; iind1 < call_ref.second.v.size(); ++iind1) {
        if (call_ref.second.v.at(iind1) == vind1) {
          // Add contribution to block row
          for (size_t iind2 = 0; iind2 < call_ref.second.v.size(); ++iind2) {
            // Corresponding index in v
            size_t vind2 = call_ref.second.v[iind2];
            if (vind2 == size_t(-1)) continue;
            // Hessian contribution
            MX H_contr = call_ref.second.hess(iind1, iind2);
            // Insert new block or add to existing one
            auto it = hess_brow.find(vind2);
            if (it != hess_brow.end()) {
              it->second += H_contr;
            } else {
              hess_brow[vind2] = H_contr;
            }
          }
          // An index can only appear once
          break;
        }
      }
    }
    // If no blocks, skip row
    if (hess_brow.empty()) continue;
    // Add empty rows to vblocks, if any
    if (voffset_last != voffset_begin) {
      vblocks.push_back(MX(voffset_begin - voffset_last, h_offsets.back()));
    }
    // Collect horizontal blocks
    hblocks.clear();
    casadi_int hoffset = 0;
    for (auto e : hess_brow) {
      // Add empty block before Jacobian block, if needed
      if (hoffset < h_offsets.at(e.first))
        hblocks.push_back(MX(vref.numel(), h_offsets.at(e.first) - hoffset));
      // Add Jacobian block
      hblocks.push_back(e.second);
      // Update offsets
      hoffset = h_offsets.at(e.first + 1);
    }
    // Add trailing empty block, if needed
    if (hoffset < h_offsets.back())
      hblocks.push_back(MX(vref.numel(), h_offsets.back() - hoffset));
    // Add new block row to vblocks
    vblocks.push_back(horzcat(hblocks));
    // Keep track of the offset handled in jac_brow
    voffset_last = voffset_end;
  }
  // Add empty trailing row to vblocks, if any
  if (voffset_last != voffset_end) {
    vblocks.push_back(MX(voffset_end - voffset_last, h_offsets.back()));
  }
  // Return additional term in jac_vdef_v
  return vertcat(vblocks);
}

void DaeBuilderInternal::clear_cache() const {
  for (bool sx : {false, true}) {
    for (bool elim_w : {false, true}) {
      for (bool lifted_calls : {false, true}) {
        Function& fref = oracle_[sx][elim_w][lifted_calls];
        if (!fref.is_null()) fref = Function();
      }
    }
  }
  clear_cache_ = false;
}

const Function& DaeBuilderInternal::oracle(bool sx, bool elim_w, bool lifted_calls) const {
  // Clear cache now, if necessary
  if (clear_cache_) clear_cache();
  // Create an MX oracle, if needed
  if (oracle_[false][elim_w][lifted_calls].is_null()) {
    // Oracle function inputs and outputs
    std::vector<MX> f_in, f_out, v;
    std::vector<std::string> f_in_name, f_out_name;
    // Index for wdef
    casadi_int wdef_ind = -1;
    // Options consistency check
    casadi_assert(!(elim_w && lifted_calls), "Incompatible options");
    // Do we need to substitute out v
    bool subst_v = false;
    // Collect all DAE input variables with at least one entry
    for (casadi_int i = 0; i != DAE_BUILDER_NUM_IN; ++i) {
      v = input(static_cast<DaeBuilderInternalIn>(i));
      if (!v.empty()) {
        if (elim_w && i == DAE_BUILDER_W) {
          subst_v = true;
        } else {
          f_in.push_back(vertcat(v));
          f_in_name.push_back(to_string(static_cast<DaeBuilderInternalIn>(i)));
        }
      }
    }
    // Collect all DAE output variables with at least one entry
    for (casadi_int i = 0; i != DAE_BUILDER_NUM_OUT; ++i) {
      v = output(static_cast<DaeBuilderInternalOut>(i));
      if (!v.empty()) {
        if (i == DAE_BUILDER_WDEF) wdef_ind = f_out.size();
        f_out.push_back(vertcat(v));
        f_out_name.push_back(to_string(static_cast<DaeBuilderInternalOut>(i)));
      }
    }
    // Eliminate v from inputs
    if (subst_v) {
      // Dependent variable definitions
      std::vector<MX> wdef = this->wdef();
      // Perform in-place substitution
      substitute_inplace(w_, wdef, f_out, false);
    } else if (lifted_calls && wdef_ind >= 0) {
      // Dependent variable definitions
      std::vector<MX> wdef = this->wdef();
      // Remove references to call nodes
      for (MX& wdefref : wdef) {
        if (wdefref.is_output()) wdefref = MX::zeros(wdefref.sparsity());
      }
      // Save to oracle outputs
      f_out.at(wdef_ind) = vertcat(wdef);
    }
    // Create oracle
    oracle_[false][elim_w][lifted_calls]
      = Function("mx_oracle", f_in, f_out, f_in_name, f_out_name);
  }
  // Return MX oracle, if requested
  if (!sx) return oracle_[false][elim_w][lifted_calls];
  // Create SX oracle, if needed
  Function& sx_oracle = oracle_[true][elim_w][lifted_calls];
  if (sx_oracle.is_null()) sx_oracle = oracle_[false][elim_w][lifted_calls].expand("sx_oracle");
  // Return SX oracle reference
  return sx_oracle;
}

void DaeBuilderInternal::CallIO::calc_jac() {
  // Consistency checks
  for (casadi_int i = 0; i < this->f.n_in(); ++i) {
    casadi_assert(this->f.size_in(i) == this->arg.at(i).size(), "Call input not provided");
  }
  for (casadi_int i = 0; i < this->f.n_out(); ++i) {
    casadi_assert(this->f.size_out(i) == this->res.at(i).size(), "Call output not provided");
  }
  // Get/generate the (cached) Jacobian function
  // casadi_message("Retrieving the Jacobian of " + str(this->f));
  this->J = this->f.jacobian();
  // casadi_message("Retrieving the Jacobian of " + str(this->f) + " done");
  // Input expressions for the call to J
  std::vector<MX> call_in = this->arg;
  call_in.insert(call_in.end(), this->res.begin(), this->res.end());
  // Create expressions for Jacobian blocks and save to struct
  this->jac_res = this->J(call_in);
}

void DaeBuilderInternal::CallIO::calc_grad() {
  // Consistency checks
  for (casadi_int i = 0; i < this->f.n_in(); ++i) {
    casadi_assert(this->f.size_in(i) == this->arg.at(i).size(), "Call input not provided");
  }
  casadi_assert(this->adj1_arg.size() == this->res.size(), "Input 'lam_vdef' not provided");
  for (casadi_int i = 0; i < this->f.n_out(); ++i) {
    casadi_assert(this->f.size_out(i) == this->res.at(i).size(), "Call output not provided");
    casadi_assert(this->adj1_arg.at(i).size() == this->res.at(i).size(),
      "Call adjoint seed not provided");
  }
  // We should make use of the Jacobian blocks here, if available
  if (!this->jac_res.empty())
    casadi_warning("Jacobian blocks currently not reused for gradient calculation");
  // Get/generate the (cached) adjoint function
  // casadi_message("Retrieving the gradient of " + str(this->f));
  this->adj1_f = this->f.reverse(1);
  // casadi_message("Retrieving the gradient of " + str(this->f) + " done");
  // Input expressions for the call to adj1_f
  std::vector<MX> call_in = this->arg;
  call_in.insert(call_in.end(), this->res.begin(), this->res.end());
  call_in.insert(call_in.end(), this->adj1_arg.begin(), this->adj1_arg.end());
  // Create expressions for adjoint sweep and save to struct
  this->adj1_res = this->adj1_f(call_in);
}

void DaeBuilderInternal::CallIO::calc_hess() {
  // Calculate gradient, if needed
  if (this->adj1_f.is_null()) calc_grad();
  // Get/generate the (cached) Hessian function
  // casadi_message("Retrieving the Hessian of " + str(this->f));
  this->H = this->adj1_f.jacobian();
  // casadi_message("Retrieving the Hessian of " + str(this->f) + " done");
  // Input expressions for the call to H
  std::vector<MX> call_in = this->arg;
  call_in.insert(call_in.end(), this->res.begin(), this->res.end());
  call_in.insert(call_in.end(), this->adj1_arg.begin(), this->adj1_arg.end());
  call_in.insert(call_in.end(), this->adj1_res.begin(), this->adj1_res.end());
  // Create expressions for Hessian blocks and save to struct
  this->hess_res = this->H(call_in);
}

const MX& DaeBuilderInternal::CallIO::jac(casadi_int oind, casadi_int iind) const {
  // Flat index
  casadi_int ind = iind + oind * this->arg.size();
  // Return reference
  return this->jac_res.at(ind);
}

const MX& DaeBuilderInternal::CallIO::hess(casadi_int iind1, casadi_int iind2) const {
  // Flat index
  casadi_int ind = iind1 + iind1 * this->adj1_arg.size();
  // Return reference
  return this->hess_res.at(ind);
}

void DaeBuilderInternal::sort_dependent(std::vector<MX>& v, std::vector<MX>& vdef) {
  // Form function to evaluate dependent variables
  Function vfcn("vfcn", {vertcat(v)}, {vertcat(vdef)}, {"v"}, {"vdef"});
  // Is any variable vector-valued?
  bool any_vector_valued = false;
  for (const MX& v_i : v) {
    casadi_assert(!v_i.is_empty(), "Cannot have zero-dimension dependent variables");
    if (!v_i.is_scalar()) {
      any_vector_valued = true;
      break;
    }
  }
  // If vector-valued variables exists, collapse them
  if (any_vector_valued) {
    // New v corresponding to one scalar input per v argument
    std::vector<MX> vfcn_in(v), vfcn_arg(v);
    for (size_t i = 0; i < v.size(); ++i) {
      if (!v.at(i).is_scalar()) {
        vfcn_in.at(i) = MX::sym(v.at(i).name());
        vfcn_arg.at(i) = repmat(vfcn_in.at(i), v.at(i).size1());
      }
    }
    // Wrap vfcn
    std::vector<MX> vfcn_out = vfcn(vertcat(vfcn_arg));
    vfcn_out = vertsplit(vfcn_out.at(0), offset(v));
    // Collapse vector-valued outputs
    for (size_t i = 0; i < v.size(); ++i) {
      if (!v.at(i).is_scalar()) {
        vfcn_out.at(i) = dot(vfcn_out.at(i), vfcn_out.at(i));
      }
    }
    // Recreate vfcn with smaller dimensions
    vfcn = Function(vfcn.name(), {vertcat(vfcn_in)}, {vertcat(vfcn_out)},
      vfcn.name_in(), vfcn.name_out());
  }
  // Calculate sparsity pattern of dvdef/dv
  Sparsity Jv = vfcn.jac_sparsity(0, 0);
  // Add diagonal (equation is v-vdef = 0)
  Jv = Jv + Sparsity::diag(Jv.size1());
  // If lower triangular, nothing to do
  if (Jv.is_triu()) return;
  // Perform a Dulmage-Mendelsohn decomposition
  std::vector<casadi_int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
  (void)Jv.btf(rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock);
  // Reorder the variables
  std::vector<MX> tmp(v.size());
  for (size_t k = 0; k < v.size(); ++k) tmp[k] = v.at(colperm.at(k));
  std::copy(tmp.begin(), tmp.end(), v.begin());
  // Reorder the equations
  for (size_t k = 0; k < v.size(); ++k) tmp[k] = vdef.at(rowperm.at(k));
  std::copy(tmp.begin(), tmp.end(), vdef.begin());
}

MX Variable::attribute(Attribute att) const {
  switch (att) {
  case MIN:
    return this->min;
  case MAX:
    return this->max;
  case NOMINAL:
    return this->nominal;
  case START:
    return this->start;
  default:
    casadi_error("Cannot process attribute '" + to_string(att) + "'");
    return MX();
  }
}

Function DaeBuilderInternal::attribute_fun(const std::string& fname,
    const std::vector<std::string>& s_in,
    const std::vector<std::string>& s_out) const {
  // Convert inputs to enums
  std::vector<DaeBuilderInternalIn> v_in;
  v_in.reserve(v_in.size());
  for (const std::string& s : s_in) v_in.push_back(to_enum<DaeBuilderInternalIn>(s));
  // Convert outputs to enums
  std::vector<Variable::Attribute> a_out;
  std::vector<DaeBuilderInternalIn> v_out;
  a_out.reserve(s_out.size());
  v_out.reserve(s_out.size());
  for (const std::string& s : s_out) {
    // Locate the underscore divider
    size_t pos = s.find('_');
    casadi_assert(pos < s.size(), "Cannot process \"" + s + "\"");
    // Get attribute
    a_out.push_back(to_enum<Variable::Attribute>(s.substr(0, pos)));
    // Get variable
    v_out.push_back(to_enum<DaeBuilderInternalIn>(s.substr(pos + 1, std::string::npos)));
  }
  // Collect input expressions
  std::vector<MX> f_in;
  f_in.reserve(s_in.size());
  for (DaeBuilderInternalIn v : v_in) f_in.push_back(vertcat(input(v)));
  // Collect output expressions
  std::vector<MX> f_out;
  f_out.reserve(s_out.size());
  for (size_t i = 0; i < s_out.size(); ++i) {
    // Get expressions for which attributes are requested
    std::vector<MX> vars = input(v_out.at(i));
    // Expressions for the attributes
    std::vector<MX> attr;
    attr.reserve(vars.size());
    // Collect attributes
    for (const MX& vi : vars)
      attr.push_back(variable(vi.name()).attribute(a_out.at(i)));
    // Add to output expressions
    f_out.push_back(vertcat(attr));
    // Handle dependency on w
    if (depends_on(f_out.back(), vertcat(w_))) {
      // Make copies of w and wdef and sort
      std::vector<MX> w_sorted = w_, wdef = this->wdef();
      sort_dependent(w_sorted, wdef);
      // Eliminate dependency on w
      substitute_inplace(w_sorted, wdef, attr, false);
      // Update f_out
      f_out.back() = vertcat(attr);
    }
    // Handle interdependencies
    if (depends_on(f_out.back(), vertcat(vars))) {
      // Make copies of vars and attr and sort
      std::vector<MX> vars_sorted = vars, attr_sorted = attr;
      sort_dependent(vars_sorted, attr_sorted);
      // Eliminate interdependencies
      substitute_inplace(vars_sorted, attr_sorted, attr, false);
      // Update f_out
      f_out.back() = vertcat(attr);
    }
  }
  // Assemble return function
  return Function(fname, f_in, f_out, s_in, s_out);
}

Function DaeBuilderInternal::dependent_fun(const std::string& fname,
    const std::vector<std::string>& s_in,
    const std::vector<std::string>& s_out) const {
  // Are we calculating d and/or w
  bool calc_d = false, calc_w = false;
  // Convert outputs to enums
  std::vector<DaeBuilderInternalIn> v_out;
  v_out.reserve(v_out.size());
  for (const std::string& s : s_out) {
    DaeBuilderInternalIn e = to_enum<DaeBuilderInternalIn>(s);
    if (e == DAE_BUILDER_D) {
      calc_d = true;
    } else if (e == DAE_BUILDER_W) {
      calc_w = true;
    } else {
      casadi_error("Can only calculate d and/or w");
    }
    v_out.push_back(e);
  }
  // Consistency check
  casadi_assert(calc_d || calc_w, "Nothing to calculate");
  // Convert inputs to enums
  std::vector<DaeBuilderInternalIn> v_in;
  v_in.reserve(v_in.size());
  for (const std::string& s : s_in) {
    DaeBuilderInternalIn e = to_enum<DaeBuilderInternalIn>(s);
    if (calc_d && e == DAE_BUILDER_D) casadi_error("'d' cannot be both input and output");
    if (calc_w && e == DAE_BUILDER_W) casadi_error("'w' cannot be both input and output");
    v_in.push_back(e);
  }
  // Collect input expressions
  std::vector<MX> f_in;
  f_in.reserve(s_in.size());
  for (DaeBuilderInternalIn v : v_in) f_in.push_back(vertcat(input(v)));
  // Collect output expressions
  std::vector<MX> f_out;
  f_out.reserve(s_out.size());
  for (DaeBuilderInternalIn v : v_out) f_out.push_back(vertcat(input(v)));
  // Variables to be substituted
  std::vector<MX> dw, dwdef;
  if (calc_d) {
    dw.insert(dw.end(), d_.begin(), d_.end());
    std::vector<MX> ddef = this->ddef();
    dwdef.insert(dwdef.end(), ddef.begin(), ddef.end());
  }
  if (calc_w) {
    dw.insert(dw.end(), w_.begin(), w_.end());
    std::vector<MX> wdef = this->wdef();
    dwdef.insert(dwdef.end(), wdef.begin(), wdef  .end());
  }
  // Perform elimination
  substitute_inplace(dw, dwdef, f_out);
  // Assemble return function
  return Function(fname, f_in, f_out, s_in, s_out);
}

Function DaeBuilderInternal::gather_eq() const {
  // Output expressions
  std::vector<MX> f_out;
  // Names of outputs
  std::vector<std::string> f_out_name;
  // Get all expressions
  for (casadi_int i = 0; i != DAE_BUILDER_NUM_OUT; ++i) {
    std::vector<MX> v = output(static_cast<DaeBuilderInternalOut>(i));
    if (!v.empty()) {
      f_out.push_back(vertcat(v));
      f_out_name.push_back(to_string(static_cast<DaeBuilderInternalOut>(i)));
    }
  }
  // Construct function
  return Function("all_eq", {}, f_out, {}, f_out_name);
}

std::vector<MX> DaeBuilderInternal::cdef() const {
  std::vector<MX> ret;
  ret.reserve(c_.size());
  for (const MX& c : c_) ret.push_back(variable(c.name()).beq);
  return ret;
}

std::vector<MX> DaeBuilderInternal::ddef() const {
  std::vector<MX> ret;
  ret.reserve(d_.size());
  for (const MX& d : d_) ret.push_back(variable(d.name()).beq);
  return ret;
}

std::vector<MX> DaeBuilderInternal::wdef() const {
  std::vector<MX> ret;
  ret.reserve(w_.size());
  for (const MX& w : w_) ret.push_back(variable(w.name()).beq);
  return ret;
}

std::vector<MX> DaeBuilderInternal::ydef() const {
  std::vector<MX> ret;
  ret.reserve(y_.size());
  for (const MX& y : y_) ret.push_back(variable(y.name()).beq);
  return ret;
}

MX DaeBuilderInternal::add_t(const std::string& name) {
  casadi_assert(t_.empty(), "'t' already defined");
  Variable v(name);
  v.v = MX::sym(name);
  v.causality = Variable::INDEPENDENT;
  add_variable(name, v);
  t_.push_back(v.v);
  return v.v;
}

MX DaeBuilderInternal::add_p(const std::string& name, casadi_int n) {
  Variable v(name);
  v.v = MX::sym(name, n);
  v.variability = Variable::FIXED;
  v.causality = Variable::INPUT;
  add_variable(name, v);
  p_.push_back(v.v);
  return v.v;
}

MX DaeBuilderInternal::add_u(const std::string& name, casadi_int n) {
  Variable v(name);
  v.v = MX::sym(name, n);
  v.variability = Variable::CONTINUOUS;
  v.causality = Variable::INPUT;
  add_variable(name, v);
  u_.push_back(v.v);
  return v.v;
}

MX DaeBuilderInternal::add_x(const std::string& name, casadi_int n) {
  Variable v(name);
  v.v = MX::sym(name, n);
  v.variability = Variable::CONTINUOUS;
  v.causality = Variable::LOCAL;
  add_variable(name, v);
  x_.push_back(v.v);
  return v.v;
}

MX DaeBuilderInternal::add_z(const std::string& name, casadi_int n) {
  Variable v(name);
  v.v = MX::sym(name, n);
  v.variability = Variable::CONTINUOUS;
  v.causality = Variable::LOCAL;
  add_variable(name, v);
  z_.push_back(v.v);
  return v.v;
}

MX DaeBuilderInternal::add_q(const std::string& name, casadi_int n) {
  Variable v(name);
  v.v = MX::sym(name, n);
  v.variability = Variable::CONTINUOUS;
  v.causality = Variable::LOCAL;
  add_variable(name, v);
  q_.push_back(v.v);
  return v.v;
}

MX DaeBuilderInternal::add_c(const std::string& name, const MX& new_cdef) {
  Variable v(name);
  v.v = MX::sym(name);
  v.variability = Variable::CONSTANT;
  v.beq = new_cdef;
  add_variable(name, v);
  c_.push_back(v.v);
  return v.v;
}

MX DaeBuilderInternal::add_d(const std::string& name, const MX& new_ddef) {
  Variable v(name);
  v.v = MX::sym(name);
  v.variability = Variable::FIXED;
  v.causality = Variable::CALCULATED_PARAMETER;
  v.beq = new_ddef;
  d_.push_back(v.v);
  return v.v;
}

MX DaeBuilderInternal::add_w(const std::string& name, const MX& new_wdef) {
  Variable v(name);
  v.v = MX::sym(name);
  v.variability = Variable::CONTINUOUS;
  v.beq = new_wdef;
  w_.push_back(v.v);
  return v.v;
}

MX DaeBuilderInternal::add_y(const std::string& name, const MX& new_ydef) {
  Variable v(name);
  v.v = MX::sym(name);
  v.causality = Variable::OUTPUT;
  v.beq = new_ydef;
  y_.push_back(v.v);
  return v.v;
}

} // namespace casadi
