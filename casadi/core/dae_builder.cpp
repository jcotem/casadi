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


#include "dae_builder.hpp"

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

  DaeBuilder::DaeBuilder() {
    this->t = MX::sym("t");
  }

  void DaeBuilder::parse_fmi(const std::string& filename) {

    // Load
    XmlFile xml_file("tinyxml");
    XmlNode document = xml_file.parse(filename);

    // **** Add model variables ****
    {
      // Get a reference to the ModelVariables node
      const XmlNode& modvars = document[0]["ModelVariables"];

      // Add variables
      for (casadi_int i=0; i<modvars.size(); ++i) {

        // Get a reference to the variable
        const XmlNode& vnode = modvars[i];

        // Get the attributes
        std::string name        = vnode.getAttribute("name");
        casadi_int valueReference;
        vnode.readAttribute("valueReference", valueReference);
        std::string variability = vnode.getAttribute("variability");
        std::string causality   = vnode.getAttribute("causality");
        std::string alias       = vnode.getAttribute("alias");

        // Skip to the next variable if its an alias
        if (alias == "alias" || alias == "negatedAlias")
          continue;

        // Get the name
        const XmlNode& nn = vnode["QualifiedName"];
        std::string qn = qualified_name(nn);

        // Add variable, if not already added
        if (varmap_.find(qn)==varmap_.end()) {

          // Create variable
          Variable var(name);

          // Value reference
          var.valueReference = valueReference;

          // Variability
          if (variability=="constant")
            var.variability = CONSTANT;
          else if (variability=="parameter")
            var.variability = PARAMETER;
          else if (variability=="discrete")
            var.variability = DISCRETE;
          else if (variability=="continuous")
            var.variability = CONTINUOUS;
          else
            throw CasadiException("Unknown variability");

          // Causality
          if (causality=="input")
            var.causality = INPUT;
          else if (causality=="output")
            var.causality = OUTPUT;
          else if (causality=="internal")
            var.causality = INTERNAL;
          else
            throw CasadiException("Unknown causality");

          // Alias
          if (alias=="noAlias")
            var.alias = NO_ALIAS;
          else if (alias=="alias")
            var.alias = ALIAS;
          else if (alias=="negatedAlias")
            var.alias = NEGATED_ALIAS;
          else
            throw CasadiException("Unknown alias");

          // Other properties
          if (vnode.hasChild("Real")) {
            const XmlNode& props = vnode["Real"];
            props.readAttribute("unit", var.unit, false);
            props.readAttribute("displayUnit", var.display_unit, false);
            props.readAttribute("min", var.min, false);
            props.readAttribute("max", var.max, false);
            props.readAttribute("initialGuess", var.guess, false);
            props.readAttribute("start", var.start, false);
            props.readAttribute("nominal", var.nominal, false);
            props.readAttribute("free", var.free, false);
          }

          // Variable category
          if (vnode.hasChild("VariableCategory")) {
            std::string cat = vnode["VariableCategory"].getText();
            if (cat=="derivative")
              var.category = CAT_DERIVATIVE;
            else if (cat=="state")
              var.category = CAT_STATE;
            else if (cat=="dependentConstant")
              var.category = CAT_DEPENDENT_CONSTANT;
            else if (cat=="independentConstant")
              var.category = CAT_INDEPENDENT_CONSTANT;
            else if (cat=="dependentParameter")
              var.category = CAT_DEPENDENT_PARAMETER;
            else if (cat=="independentParameter")
              var.category = CAT_INDEPENDENT_PARAMETER;
            else if (cat=="algebraic")
              var.category = CAT_ALGEBRAIC;
            else
              throw CasadiException("Unknown variable category: " + cat);
          }

          // Add to list of variables
          add_variable(qn, var);

          // Sort expression
          switch (var.category) {
          case CAT_DERIVATIVE:
            // Skip - meta information about time derivatives is
            //        kept together with its parent variable
            break;
          case CAT_STATE:
            this->s.push_back(var.v);
            this->sdot.push_back(var.d);
            break;
          case CAT_DEPENDENT_CONSTANT:
            // Skip
            break;
          case CAT_INDEPENDENT_CONSTANT:
            // Skip
            break;
          case CAT_DEPENDENT_PARAMETER:
            // Skip
            break;
          case CAT_INDEPENDENT_PARAMETER:
            if (var.free) {
              this->p.push_back(var.v);
            } else {
              // Skip
            }
            break;
          case CAT_ALGEBRAIC:
            if (var.causality == INTERNAL) {
              this->s.push_back(var.v);
              this->sdot.push_back(var.d);
            } else if (var.causality == INPUT) {
              this->u.push_back(var.v);
            }
            break;
          default:
            casadi_error("Unknown category");
          }
        }
      }
    }

    // **** Add binding equations ****
    {
      // Get a reference to the BindingEquations node
      const XmlNode& bindeqs = document[0]["equ:BindingEquations"];

      for (casadi_int i=0; i<bindeqs.size(); ++i) {
        const XmlNode& beq = bindeqs[i];

        // Get the variable and binding expression
        Variable& var = read_variable(beq[0]);
        MX bexpr = read_expr(beq[1][0]);
        this->v.push_back(var.v);
        this->vdef.push_back(bexpr);
      }
    }

    // **** Add dynamic equations ****
    {
      // Get a reference to the DynamicEquations node
      const XmlNode& dyneqs = document[0]["equ:DynamicEquations"];

      // Add equations
      for (casadi_int i=0; i<dyneqs.size(); ++i) {

        // Get a reference to the variable
        const XmlNode& dnode = dyneqs[i];

        // Add the differential equation
        MX de_new = read_expr(dnode[0]);
        this->dae.push_back(de_new);
      }
    }

    // **** Add initial equations ****
    {
      // Get a reference to the DynamicEquations node
      const XmlNode& initeqs = document[0]["equ:InitialEquations"];

      // Add equations
      for (casadi_int i=0; i<initeqs.size(); ++i) {

        // Get a reference to the node
        const XmlNode& inode = initeqs[i];

        // Add the differential equations
        for (casadi_int i=0; i<inode.size(); ++i) {
          this->init.push_back(read_expr(inode[i]));
        }
      }
    }

    // **** Add optimization ****
    if (document[0].hasChild("opt:Optimization")) {

      // Get a reference to the DynamicEquations node
      const XmlNode& opts = document[0]["opt:Optimization"];
      for (casadi_int i=0; i<opts.size(); ++i) {

        // Get a reference to the node
        const XmlNode& onode = opts[i];

        // Get the type
        if (onode.checkName("opt:ObjectiveFunction")) { // mayer term
          try {
            // Add components
            for (casadi_int i=0; i<onode.size(); ++i) {
              const XmlNode& var = onode[i];

              // If string literal, ignore
              if (var.checkName("exp:StringLiteral"))
                continue;

              // Read expression
              MX v = read_expr(var);

              // Treat as an output
              add_y("mterm", v);
            }
          } catch(std::exception& ex) {
            throw CasadiException(std::string("addObjectiveFunction failed: ") + ex.what());
          }
        } else if (onode.checkName("opt:IntegrandObjectiveFunction")) {
          try {
            for (casadi_int i=0; i<onode.size(); ++i) {
              const XmlNode& var = onode[i];

              // If string literal, ignore
              if (var.checkName("exp:StringLiteral")) continue;

              // Read expression
              MX v = read_expr(var);

              // Treat as a quadrature state
              add_q("lterm");
              add_quad("lterm_rhs", v);
            }
          } catch(std::exception& ex) {
            throw CasadiException(std::string("addIntegrandObjectiveFunction failed: ")
                                  + ex.what());
          }
        } else if (onode.checkName("opt:IntervalStartTime")) {
          // Ignore, treated above
        } else if (onode.checkName("opt:IntervalFinalTime")) {
          // Ignore, treated above
        } else if (onode.checkName("opt:TimePoints")) {
          // Ignore, treated above
        } else if (onode.checkName("opt:PointConstraints")) {
          casadi_warning("opt:PointConstraints not supported, ignored");
        } else if (onode.checkName("opt:Constraints")) {
          casadi_warning("opt:Constraints not supported, ignored");
        } else if (onode.checkName("opt:PathConstraints")) {
          casadi_warning("opt:PointConstraints not supported, ignored");
        } else {
          casadi_warning("DaeBuilder::addOptimization: Unknown node " + str(onode.name()));
        }
      }
    }

    // Make sure that the dimensions are consistent at this point
    if (this->s.size()!=this->dae.size()) {
      casadi_warning("The number of differential-algebraic equations does not match "
                     "the number of implicitly defined states.");
    }
    if (this->z.size()!=this->alg.size()) {
      casadi_warning("The number of algebraic equations (equations not involving "
                    "differentiated variables) does not match the number of "
                    "algebraic variables.");
    }
  }

  Variable& DaeBuilder::read_variable(const XmlNode& node) {
    // Qualified name
    std::string qn = qualified_name(node);

    // Find and return the variable
    return variable(qn);
  }

  MX DaeBuilder::read_expr(const XmlNode& node) {
    const std::string& fullname = node.name();
    if (fullname.find("exp:")== std::string::npos) {
      casadi_error("DaeBuilder::read_expr: unknown - expression is supposed to "
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
      const Variable& v = read_variable(node[0]);
      return v.d;
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
      return t;
    } else if (name=="TimedVariable") {
      return read_variable(node[0]).v;
    }

    // throw error if reached this point
    throw CasadiException(std::string("DaeBuilder::read_expr: Unknown node: ") + name);

  }

  void DaeBuilder::disp(std::ostream& stream, bool more) const {
    // Assert correctness
    if (more) sanity_check();

    // Print dimensions
    stream << "ns = " << this->s.size() << ", "
           << "nx = " << this->x.size() << ", "
           << "nz = " << this->z.size() << ", "
           << "nq = " << this->q.size() << ", "
           << "ny = " << this->y.size() << ", "
           << "np = " << this->p.size() << ", "
           << "nv = " << this->v.size() << ", "
           << "nu = " << this->u.size();

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
    stream << "  t = " << str(this->t) << std::endl;
    if (!this->s.empty()) stream << "  s = " << str(this->s) << std::endl;
    if (!this->x.empty()) stream << "  x = " << str(this->x) << std::endl;
    if (!this->z.empty()) stream << "  z =  " << str(this->z) << std::endl;
    if (!this->q.empty()) stream << "  q =  " << str(this->q) << std::endl;
    if (!this->y.empty()) stream << "  y =  " << str(this->y) << std::endl;
    if (!this->p.empty()) stream << "  p =  " << str(this->p) << std::endl;
    if (!this->v.empty()) stream << "  d =  " << str(this->v) << std::endl;
    if (!this->u.empty()) stream << "  u =  " << str(this->u) << std::endl;

    if (!this->v.empty()) {
      stream << "Dependent variables" << std::endl;
      for (casadi_int i=0; i<this->v.size(); ++i)
        stream << "  " << str(this->v[i]) << " == " << str(this->vdef[i]) << std::endl;
    }

    if (!this->dae.empty()) {
      stream << "Fully-implicit differential-algebraic equations" << std::endl;
      for (casadi_int k=0; k<this->dae.size(); ++k) {
        stream << "  0 == " << this->dae[k] << std::endl;
      }
    }

    if (!this->x.empty()) {
      stream << "Differential equations" << std::endl;
      for (casadi_int k=0; k<this->x.size(); ++k) {
        stream << "  " << str(der(this->x[k])) << " == " << str(this->ode[k]) << std::endl;
      }
    }

    if (!this->alg.empty()) {
      stream << "Algebraic equations" << std::endl;
      for (casadi_int k=0; k<this->z.size(); ++k) {
        stream << "  0 == " << str(this->alg[k]) << std::endl;
      }
    }

    if (!this->q.empty()) {
      stream << "Quadrature equations" << std::endl;
      for (casadi_int k=0; k<this->q.size(); ++k) {
        stream << "  " << str(der(this->q[k])) << " == " << str(this->quad[k]) << std::endl;
      }
    }

    if (!this->init.empty()) {
      stream << "Initial equations" << std::endl;
      for (casadi_int k=0; k<this->init.size(); ++k) {
        stream << "  0 == " << str(this->init[k]) << std::endl;
      }
    }

    if (!this->y.empty()) {
      stream << "Output variables" << std::endl;
      for (casadi_int i=0; i<this->y.size(); ++i) {
        stream << "  " << str(this->y[i]) << " == " << str(this->ydef[i]) << std::endl;
      }
    }
  }

  void DaeBuilder::eliminate_quad() {
    // Move all the quadratures to the list of differential states
    this->x.insert(this->x.end(), this->q.begin(), this->q.end());
    this->q.clear();
  }

  void DaeBuilder::scale_variables() {
    // Assert correctness
    sanity_check();

    // Gather variables and expressions to replace
    std::vector<MX> v_id, v_rep;
    for (VarMap::iterator it=varmap_.begin(); it!=varmap_.end(); ++it) {
      if (it->second.nominal!=1) {
        Variable& v=it->second;
        casadi_assert_dev(v.nominal!=0);
        v.min /= v.nominal;
        v.max /= v.nominal;
        v.start /= v.nominal;
        v.derivative_start /= v.nominal;
        v.guess /= v.nominal;
        v_id.push_back(v.v);
        v_id.push_back(v.d);
        v_rep.push_back(v.v * v.nominal);
        v_rep.push_back(v.d * v.nominal);
      }
    }

    // Quick return if no expressions to substitute
    if (v_id.empty()) return;

    // Collect all expressions to be replaced
    std::vector<MX> ex;
    ex.insert(ex.end(), this->ode.begin(), this->ode.end());
    ex.insert(ex.end(), this->dae.begin(), this->dae.end());
    ex.insert(ex.end(), this->alg.begin(), this->alg.end());
    ex.insert(ex.end(), this->quad.begin(), this->quad.end());
    ex.insert(ex.end(), this->vdef.begin(), this->vdef.end());
    ex.insert(ex.end(), this->ydef.begin(), this->ydef.end());
    ex.insert(ex.end(), this->init.begin(), this->init.end());

    // Substitute all at once (more efficient since they may have common subexpressions)
    ex = substitute(ex, v_id, v_rep);

    // Get the modified expressions
    std::vector<MX>::const_iterator it=ex.begin();
    for (casadi_int i=0; i<this->x.size(); ++i) this->ode[i] = *it++ / nominal(this->x[i]);
    for (casadi_int i=0; i<this->s.size(); ++i) this->dae[i] = *it++;
    for (casadi_int i=0; i<this->z.size(); ++i) this->alg[i] = *it++;
    for (casadi_int i=0; i<this->q.size(); ++i) this->quad[i] = *it++ / nominal(this->q[i]);
    for (casadi_int i=0; i<this->v.size(); ++i) this->vdef[i] = *it++ / nominal(this->v[i]);
    for (casadi_int i=0; i<this->y.size(); ++i) this->ydef[i] = *it++ / nominal(this->y[i]);
    for (casadi_int i=0; i<this->init.size(); ++i) this->init[i] = *it++;
    casadi_assert_dev(it==ex.end());

    // Nominal value is 1 after scaling
    for (VarMap::iterator it=varmap_.begin(); it!=varmap_.end(); ++it) {
      it->second.nominal=1;
    }
  }

  void DaeBuilder::sort_v() {
    // Quick return if no intermediates
    if (this->v.empty()) return;

    // Find out which intermediates depends on which other
    Function f("tmp", {vertcat(this->v)}, {vertcat(this->v) - vertcat(this->vdef)});
    Sparsity sp = f.sparsity_jac(0, 0);
    casadi_assert_dev(sp.is_square());

    // BLT transformation
    std::vector<casadi_int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    sp.btf(rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    std::vector<MX> vdefnew(this->v.size()), dnew(this->v.size());
    for (casadi_int i=0; i<colperm.size(); ++i) {
      // Permute equations
      vdefnew[i] = this->vdef[colperm[i]];

      // Permute variables
      dnew[i] = this->v[colperm[i]];
    }
    this->vdef = vdefnew;
    this->v = dnew;
  }

  void DaeBuilder::split_v() {
    // Quick return if no intermediates
    if (this->v.empty()) return;

    // Begin by sorting the dependent parameters
    sort_v();

    // Sort the equations by causality
    std::vector<MX> ex;
    substitute_inplace(this->v, this->vdef, ex);

    // Make sure that the interdependencies have been properly eliminated
    casadi_assert_dev(!depends_on(vertcat(this->vdef), vertcat(this->v)));
  }

  void DaeBuilder::eliminate_v() {
    // Quick return if possible
    if (this->v.empty()) return;

    // Begin by sorting the dependent parameters
    sort_v();

    // Collect all expressions to be replaced
    std::vector<MX> ex;
    ex.insert(ex.end(), this->ode.begin(), this->ode.end());
    ex.insert(ex.end(), this->dae.begin(), this->dae.end());
    ex.insert(ex.end(), this->alg.begin(), this->alg.end());
    ex.insert(ex.end(), this->quad.begin(), this->quad.end());
    ex.insert(ex.end(), this->ydef.begin(), this->ydef.end());
    ex.insert(ex.end(), this->init.begin(), this->init.end());

    // Substitute all at once (since they may have common subexpressions)
    substitute_inplace(this->v, this->vdef, ex);

    // Get the modified expressions
    std::vector<MX>::const_iterator it=ex.begin();
    for (casadi_int i=0; i<this->x.size(); ++i) this->ode[i] = *it++;
    for (casadi_int i=0; i<this->s.size(); ++i) this->dae[i] = *it++;
    for (casadi_int i=0; i<this->z.size(); ++i) this->alg[i] = *it++;
    for (casadi_int i=0; i<this->q.size(); ++i) this->quad[i] = *it++;
    for (casadi_int i=0; i<this->y.size(); ++i) this->ydef[i] = *it++;
    for (casadi_int i=0; i<this->init.size(); ++i) this->init[i] = *it++;
    casadi_assert_dev(it==ex.end());
  }

  void DaeBuilder::scale_equations() {
    casadi_error("DaeBuilder::scale_equations broken");
  }

  void DaeBuilder::sort_dae() {
    // Quick return if no differential states
    if (this->x.empty()) return;

    // Find out which differential equation depends on which differential state
    Function f("tmp", {vertcat(this->sdot)}, {vertcat(this->dae)});
    Sparsity sp = f.sparsity_jac(0, 0);
    casadi_assert_dev(sp.is_square());

    // BLT transformation
    std::vector<casadi_int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    sp.btf(rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    std::vector<MX> daenew(this->s.size()), snew(this->s.size()), sdotnew(this->s.size());
    for (casadi_int i=0; i<rowperm.size(); ++i) {
      // Permute equations
      daenew[i] = this->dae[rowperm[i]];

      // Permute variables
      snew[i] = this->s[colperm[i]];
      sdotnew[i] = this->sdot[colperm[i]];
    }
    this->dae = daenew;
    this->s = snew;
    this->sdot = sdotnew;
  }

  void DaeBuilder::sort_alg() {
    // Quick return if no algebraic states
    if (this->z.empty()) return;

    // Find out which algebraic equation depends on which algebraic state
    Function f("tmp", {vertcat(this->z)}, {vertcat(this->alg)});
    Sparsity sp = f.sparsity_jac(0, 0);
    casadi_assert_dev(sp.is_square());

    // BLT transformation
    std::vector<casadi_int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    sp.btf(rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    std::vector<MX> algnew(this->z.size()), znew(this->z.size());
    for (casadi_int i=0; i<rowperm.size(); ++i) {
      // Permute equations
      algnew[i] = this->alg[rowperm[i]];

      // Permute variables
      znew[i] = this->z[colperm[i]];
    }
    this->alg = algnew;
    this->z = znew;
  }

  void DaeBuilder::make_semi_explicit() {
    // Only works if there are no i
    eliminate_v();

    // Separate the algebraic variables and equations
    split_dae();

    // Quick return if there are no implicitly defined states
    if (this->s.empty()) return;

    // Write the ODE as a function of the state derivatives
    Function f("tmp", {vertcat(this->sdot)}, {vertcat(this->dae)});

    // Get the sparsity of the Jacobian which can be used to determine which
    // variable can be calculated from which other
    Sparsity sp = f.sparsity_jac(0, 0);
    casadi_assert_dev(sp.is_square());

    // BLT transformation
    std::vector<casadi_int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    casadi_int nb = sp.btf(rowperm, colperm, rowblock, colblock,
                                  coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    std::vector<MX> daenew(this->s.size()), snew(this->s.size()), sdotnew(this->s.size());
    for (casadi_int i=0; i<rowperm.size(); ++i) {
      // Permute equations
      daenew[i] = this->dae[rowperm[i]];

      // Permute variables
      snew[i] = this->s[colperm[i]];
      sdotnew[i] = this->sdot[colperm[i]];
    }
    this->dae = daenew;
    this->s = snew;
    this->sdot = sdotnew;

    // Differentiate to write the sorted ODE as a function of state derivatives
    MX J = jacobian(vertcat(this->dae), vertcat(this->sdot));

    // Explicit ODE
    std::vector<MX> new_ode;

    // Loop over blocks
    for (casadi_int b=0; b<nb; ++b) {

      // Get variables in the block
      std::vector<MX> xb(this->s.begin()+colblock[b], this->s.begin()+colblock[b+1]);
      std::vector<MX> xdotb(this->sdot.begin()+colblock[b], this->sdot.begin()+colblock[b+1]);

      // Get equations in the block
      std::vector<MX> fb(this->dae.begin()+rowblock[b], this->dae.begin()+rowblock[b+1]);

      // Get local Jacobian
      MX Jb = J(Slice(rowblock[b], rowblock[b+1]), Slice(colblock[b], colblock[b+1])); // NOLINT

      // If Jb depends on xb, then the state derivative does not enter linearly
      // in the ODE and we cannot solve for the state derivative
      casadi_assert(!depends_on(Jb, vertcat(xdotb)),
        "Cannot find an explicit expression for variable(s) " + str(xb));

      // Divide fb into a part which depends on vb and a part which doesn't according to
      // "fb == mul(Jb, vb) + fb_res"
      std::vector<MX> fb_res = substitute(fb, xdotb, std::vector<MX>(xdotb.size(), 0));

      // Solve for vb
      std::vector<MX> fb_exp = vertsplit(solve(Jb, -vertcat(fb_res)));

      // Add to explicitly determined equations and variables
      new_ode.insert(new_ode.end(), fb_exp.begin(), fb_exp.end());
    }

    // Eliminate inter-dependencies
    std::vector<MX> ex;
    substitute_inplace(this->sdot, new_ode, ex, false);

    // Add to explicit differential states and ODE
    this->x.insert(this->x.end(), this->s.begin(), this->s.end());
    this->ode.insert(this->ode.end(), new_ode.begin(), new_ode.end());
    this->dae.clear();
    this->s.clear();
    this->sdot.clear();
  }

  void DaeBuilder::eliminate_alg() {
    // Only works if there are no i
    eliminate_v();

    // Quick return if there are no algebraic states
    if (this->z.empty()) return;

    // Write the algebraic equations as a function of the algebraic states
    Function f("f", {vertcat(this->z)}, {vertcat(this->alg)});

    // Get the sparsity of the Jacobian which can be used to determine which
    // variable can be calculated from which other
    Sparsity sp = f.sparsity_jac(0, 0);
    casadi_assert_dev(sp.is_square());

    // BLT transformation
    std::vector<casadi_int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    casadi_int nb = sp.btf(rowperm, colperm, rowblock, colblock,
                                  coarse_rowblock, coarse_colblock);

    // Resort equations and variables
    std::vector<MX> algnew(this->z.size()), znew(this->z.size());
    for (casadi_int i=0; i<rowperm.size(); ++i) {
      // Permute equations
      algnew[i] = this->alg[rowperm[i]];

      // Permute variables
      znew[i] = this->z[colperm[i]];
    }
    this->alg = algnew;
    this->z = znew;

    // Rewrite the sorted algebraic equations as a function of the algebraic states
    f = Function("f", {vertcat(this->z)}, {vertcat(this->alg)});

    // Variables where we have found an explicit expression and where we haven't
    std::vector<MX> z_exp, z_imp;

    // Explicit and implicit equations
    std::vector<MX> f_exp, f_imp;

    // Loop over blocks
    for (casadi_int b=0; b<nb; ++b) {

      // Get local variables
      std::vector<MX> zb(this->z.begin()+colblock[b], this->z.begin()+colblock[b+1]);

      // Get local equations
      std::vector<MX> fb(this->alg.begin()+rowblock[b], this->alg.begin()+rowblock[b+1]);

      // Get local Jacobian
      MX Jb = jacobian(vertcat(fb), vertcat(zb));

      // If Jb depends on zb, then we cannot (currently) solve for it explicitly
      if (depends_on(Jb, vertcat(zb))) {

        // Add the equations to the new list of algebraic equations
        f_imp.insert(f_imp.end(), fb.begin(), fb.end());

        // ... and the variables accordingly
        z_imp.insert(z_imp.end(), zb.begin(), zb.end());

      } else { // The variables that we wish to determine enter linearly

        // Divide fb into a part which depends on vb and a part which doesn't
        // according to "fb == mul(Jb, vb) + fb_res"
        std::vector<MX> fb_res = substitute(fb, zb, std::vector<MX>(zb.size(), 0));

        // Solve for vb
        std::vector<MX> fb_exp = vertsplit(solve(Jb, -vertcat(fb_res)));

        // Add to explicitly determined equations and variables
        z_exp.insert(z_exp.end(), zb.begin(), zb.end());
        f_exp.insert(f_exp.end(), fb_exp.begin(), fb_exp.end());
      }
    }

    // Eliminate inter-dependencies in fb_exp
    std::vector<MX> ex;
    substitute_inplace(z_exp, f_exp, ex, false);

    // Add to the beginning of the dependent variables
    // (since the other dependent variable might depend on them)
    this->v.insert(this->v.begin(), z_exp.begin(), z_exp.end());
    this->vdef.insert(this->vdef.begin(), f_exp.begin(), f_exp.end());

    // Save new algebraic equations
    this->z = z_imp;
    this->alg = f_imp;

    // Eliminate new dependent variables from the other equations
    eliminate_v();
  }

  void DaeBuilder::make_explicit() {
    // Only works if there are no i
    eliminate_v();

    // Start by transforming to semi-explicit form
    make_semi_explicit();

    // Then eliminate the algebraic variables
    eliminate_alg();

    // Error if still algebraic variables
    casadi_assert(this->z.empty(), "Failed to eliminate algebraic variables");
  }

  const Variable& DaeBuilder::variable(const std::string& name) const {
    return const_cast<DaeBuilder*>(this)->variable(name);
  }

  Variable& DaeBuilder::variable(const std::string& name) {
    // Find the variable
    VarMap::iterator it = varmap_.find(name);
    if (it==varmap_.end()) {
      casadi_error("No such variable: \"" + name + "\".");
    }

    // Return the variable
    return it->second;
  }

  void DaeBuilder::add_variable(const std::string& name, const Variable& var) {
    // Try to find the component
    if (varmap_.find(name)!=varmap_.end()) {
      std::stringstream ss;
      casadi_error("Variable \"" + name + "\" has already been added.");
    }
    // Add to the map of all variables
    varmap_[name] = var;
    // Clear cache
    clear_cache();
  }

  MX DaeBuilder::add_variable(const std::string& name, casadi_int n) {
    return add_variable(name, Sparsity::dense(n));
  }

  MX DaeBuilder::add_variable(const std::string& name, const Sparsity& sp) {
    Variable v(name, sp);
    add_variable(name, v);
    return v.v;
  }

  void DaeBuilder::add_variable(const MX& new_v, const MX& new_der_v) {
    Variable v(new_v.name(), new_v.sparsity(), new_v, new_der_v);
    add_variable(new_v.name(), v);
  }

  MX DaeBuilder::add_x(const std::string& name, casadi_int n) {
    if (name.empty()) return add_x("x" + str(this->x.size()), n);
    MX new_x = add_variable(name, n);
    this->x.push_back(new_x);
    return new_x;
  }

  void DaeBuilder::register_x(const MX& new_x, const MX& new_der_x) {
    add_variable(new_x, new_der_x);
    this->x.push_back(new_x);
  }

  void DaeBuilder::register_z(const MX& new_z, const MX& new_der_z) {
    add_variable(new_z, new_der_z);
    this->z.push_back(new_z);
  }

  void DaeBuilder::register_v(const MX& new_v, const MX& new_vdef, const MX& new_der_v) {
    if (new_v.sparsity() != new_vdef.sparsity())
      casadi_error("Mismatching sparsity in DaeBuilder::register_v");
    add_variable(new_v, new_der_v);
    this->v.push_back(new_v);
    this->vdef.push_back(new_vdef);
    this->lam_vdef.push_back(MX::sym("lam_" + new_v.name(), new_v.sparsity()));
  }

  void DaeBuilder::register_y(const MX& new_y, const MX& new_ydef, const MX& new_der_y) {
    if (new_y.sparsity() != new_ydef.sparsity())
      casadi_error("Mismatching sparsity in DaeBuilder::register_y");
    add_variable(new_y, new_der_y);
    this->y.push_back(new_y);
    this->ydef.push_back(new_ydef);
    this->lam_ydef.push_back(MX::sym("lam_" + new_y.name(), new_y.sparsity()));
  }

  MX DaeBuilder::add_q(const std::string& name, casadi_int n) {
    if (name.empty()) return add_q("q" + str(this->q.size()), n);
    MX new_q = add_variable(name, n);
    this->q.push_back(new_q);
    return new_q;
  }

  std::pair<MX, MX> DaeBuilder::add_s(const std::string& name, casadi_int n) {
    if (name.empty()) return add_s("s" + str(this->s.size()), n);
    Variable v(name, Sparsity::dense(n));
    add_variable(name, v);
    this->s.push_back(v.v);
    this->sdot.push_back(v.d);
    return std::pair<MX, MX>(v.v, v.d);
  }

  MX DaeBuilder::add_z(const std::string& name, casadi_int n) {
    if (name.empty()) return add_z("z" + str(this->z.size()), n);
    MX new_z = add_variable(name, n);
    this->z.push_back(new_z);
    return new_z;
  }

  MX DaeBuilder::add_p(const std::string& name, casadi_int n) {
    if (name.empty()) return add_p("p" + str(this->p.size()), n);
    MX new_p = add_variable(name, n);
    this->p.push_back(new_p);
    return new_p;
  }

  MX DaeBuilder::add_u(const std::string& name, casadi_int n) {
    if (name.empty()) return add_u("u" + str(this->u.size()), n);
    MX new_u = add_variable(name, n);
    this->u.push_back(new_u);
    return new_u;
  }

  MX DaeBuilder::add_aux(const std::string& name, casadi_int n) {
    if (name.empty()) return add_aux("aux" + str(this->aux.size()), n);
    MX new_aux = add_variable(name, n);
    this->aux.push_back(new_aux);
    return new_aux;
  }

  MX DaeBuilder::add_v(const std::string& name, const MX& new_vdef) {
    MX new_v = add_variable(name, new_vdef.sparsity());
    this->v.push_back(new_v);
    this->vdef.push_back(new_vdef);
    this->lam_vdef.push_back(MX::sym("lam_" + name, new_vdef.sparsity()));
    return new_v;
  }

  MX DaeBuilder::add_y(const std::string& name, const MX& new_ydef) {
    MX new_y = add_variable(name, new_ydef.sparsity());
    this->y.push_back(new_y);
    this->ydef.push_back(new_ydef);
    this->lam_ydef.push_back(MX::sym("lam_" + name, new_ydef.sparsity()));
    return new_y;
  }

  void DaeBuilder::add_ode(const std::string& name, const MX& new_ode) {
    this->ode.push_back(new_ode);
    this->lam_ode.push_back(MX::sym("lam_" + name, new_ode.sparsity()));
    clear_cache();
  }

  void DaeBuilder::add_dae(const std::string& name, const MX& new_dae) {
    this->dae.push_back(new_dae);
    this->lam_dae.push_back(MX::sym("lam_" + name, new_dae.sparsity()));
    clear_cache();
  }

  void DaeBuilder::add_alg(const std::string& name, const MX& new_alg) {
    this->alg.push_back(new_alg);
    this->lam_alg.push_back(MX::sym("lam_" + name, new_alg.sparsity()));
    clear_cache();
  }

  void DaeBuilder::add_quad(const std::string& name, const MX& new_quad) {
    this->quad.push_back(new_quad);
    this->lam_quad.push_back(MX::sym("lam_" + name, new_quad.sparsity()));
    clear_cache();
  }

  void DaeBuilder::sanity_check() const {
    // Time
    casadi_assert(this->t.is_symbolic(), "Non-symbolic time t");
    casadi_assert(this->t.is_scalar(), "Non-scalar time t");

    // Differential states
    casadi_assert(this->x.size()==this->ode.size(),
                          "x and ode have different lengths");
    for (casadi_int i=0; i<this->x.size(); ++i) {
      casadi_assert(this->x[i].size()==this->ode[i].size(),
                            "ode has wrong dimensions");
      casadi_assert(this->x[i].is_symbolic(), "Non-symbolic state x");
    }

    // DAE
    casadi_assert(this->s.size()==this->sdot.size(),
                          "s and sdot have different lengths");
    casadi_assert(this->s.size()==this->dae.size(),
                          "s and dae have different lengths");
    for (casadi_int i=0; i<this->s.size(); ++i) {
      casadi_assert(this->s[i].is_symbolic(), "Non-symbolic state s");
      casadi_assert(this->s[i].size()==this->sdot[i].size(),
                            "sdot has wrong dimensions");
      casadi_assert(this->s[i].size()==this->dae[i].size(),
                            "dae has wrong dimensions");
    }

    // Algebraic variables/equations
    casadi_assert(this->z.size()==this->alg.size(),
                          "z and alg have different lengths");
    for (casadi_int i=0; i<this->z.size(); ++i) {
      casadi_assert(this->z[i].is_symbolic(), "Non-symbolic algebraic variable z");
      casadi_assert(this->z[i].size()==this->alg[i].size(),
                            "alg has wrong dimensions");
    }

    // Quadrature states/equations
    casadi_assert(this->q.size()==this->quad.size(),
                          "q and quad have different lengths");
    for (casadi_int i=0; i<this->q.size(); ++i) {
      casadi_assert(this->q[i].is_symbolic(), "Non-symbolic quadrature state q");
      casadi_assert(this->q[i].size()==this->quad[i].size(),
                            "quad has wrong dimensions");
    }

    // Intermediate variables
    casadi_assert(this->v.size()==this->vdef.size(),
                          "v and vdef have different lengths");
    for (casadi_int i=0; i<this->v.size(); ++i) {
      casadi_assert(this->v[i].is_symbolic(), "Non-symbolic dependent parameter v");
      casadi_assert(this->v[i].size()==this->vdef[i].size(),
                            "vdef has wrong dimensions");
    }

    // Output equations
    casadi_assert(this->y.size()==this->ydef.size(),
                          "y and ydef have different lengths");
    for (casadi_int i=0; i<this->y.size(); ++i) {
      casadi_assert(this->y[i].is_symbolic(), "Non-symbolic output y");
      casadi_assert(this->y[i].size()==this->ydef[i].size(),
                            "ydef has wrong dimensions");
    }

    // Control
    for (casadi_int i=0; i<this->u.size(); ++i) {
      casadi_assert(this->u[i].is_symbolic(), "Non-symbolic control u");
    }

    // Parameter
    for (casadi_int i=0; i<this->p.size(); ++i) {
      casadi_assert(this->p[i].is_symbolic(), "Non-symbolic parameter p");
    }
  }

  std::string DaeBuilder::qualified_name(const XmlNode& nn) {
    // Stringstream to assemble name
    std::stringstream qn;

    for (casadi_int i=0; i<nn.size(); ++i) {
      // Add a dot
      if (i!=0) qn << ".";

      // Get the name part
      qn << nn[i].getAttribute("name");

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

  MX DaeBuilder::var(const std::string& name) const {
    return variable(name).v;
  }

  MX DaeBuilder::der(const std::string& name) const {
    return variable(name).d;
  }

  MX DaeBuilder::der(const MX& var) const {
    casadi_assert_dev(var.is_column() && var.is_symbolic());
    MX ret = MX::zeros(var.sparsity());
    for (casadi_int i=0; i<ret.nnz(); ++i) {
      ret.nz(i) = der(var.nz(i).name());
    }
    return ret;
  }

  void DaeBuilder::split_dae() {
    // Only works if there are no d
    eliminate_v();

    // Quick return if no s
    if (this->s.empty()) return;

    // We investigate the interdependencies in sdot -> dae
    Function f("f", {vertcat(this->sdot)}, {vertcat(this->dae)});

    // Number of s
    casadi_int ns = f.nnz_in(0);
    casadi_assert_dev(f.nnz_out(0)==ns);

    // Input/output buffers
    std::vector<bvec_t> f_sdot(ns, 1);
    std::vector<bvec_t> f_dae(ns, 0);

    // Propagate to f_dae
    f({get_ptr(f_sdot)}, {get_ptr(f_dae)});

    // Get the new differential and algebraic equations
    std::vector<MX> new_dae, new_alg;
    for (casadi_int i=0; i<ns; ++i) {
      if (f_dae[i]==bvec_t(1)) {
        new_dae.push_back(this->dae[i]);
      } else {
        casadi_assert_dev(f_dae[i]==bvec_t(0));
        new_alg.push_back(this->dae[i]);
      }
    }

    // Seed all outputs
    std::fill(f_dae.begin(), f_dae.end(), 1);

    // Propagate to f_sdot
    std::fill(f_sdot.begin(), f_sdot.end(), 0);
    f.rev({get_ptr(f_sdot)}, {get_ptr(f_dae)});

    // Get the new algebraic variables and new states
    std::vector<MX> new_s, new_sdot, new_z;
    for (casadi_int i=0; i<ns; ++i) {
      if (f_sdot[i]==bvec_t(1)) {
        new_s.push_back(this->s[i]);
        new_sdot.push_back(this->sdot[i]);
      } else {
        casadi_assert_dev(f_sdot[i]==bvec_t(0));
        new_z.push_back(this->s[i]);
      }
    }

    // Make sure split was successful
    casadi_assert_dev(new_dae.size()==new_s.size());

    // Divide up the s and dae
    this->dae = new_dae;
    this->s = new_s;
    this->sdot = new_sdot;
    this->alg.insert(this->alg.end(), new_alg.begin(), new_alg.end());
    this->z.insert(this->z.end(), new_z.begin(), new_z.end());
  }

  std::string DaeBuilder::unit(const std::string& name) const {
    return variable(name).unit;
  }

  std::string DaeBuilder::unit(const MX& var) const {
    casadi_assert(!var.is_column() && var.is_valid_input(),
                          "DaeBuilder::unit: Argument must be a symbolic vector");
    if (var.is_empty()) {
      return "n/a";
    } else {
      std::vector<MX> prim = var.primitives();
      std::string ret = unit(prim.at(0).name());
      for (casadi_int i=1; i<prim.size(); ++i) {
        casadi_assert(ret == unit(prim.at(i).name()),
                              "DaeBuilder::unit: Argument has mixed units");
      }
      return ret;
    }
  }

  void DaeBuilder::set_unit(const std::string& name, const std::string& val) {
    variable(name).unit = val;
  }

  double DaeBuilder::nominal(const std::string& name) const {
    return variable(name).nominal;
  }

  void DaeBuilder::set_nominal(const std::string& name, double val) {
    variable(name).nominal = val;
  }

  std::vector<double> DaeBuilder::nominal(const MX& var) const {
    casadi_assert(var.is_column() && var.is_valid_input(),
                          "DaeBuilder::nominal: Argument must be a symbolic vector");
    std::vector<double> ret(var.nnz());
    std::vector<MX> prim = var.primitives();
    for (casadi_int i=0; i<prim.size(); ++i) {
      casadi_assert_dev(prim[i].nnz()==1);
      ret[i] = nominal(prim.at(i).name());
    }
    return ret;
  }

  void DaeBuilder::set_nominal(const MX& var, const std::vector<double>& val) {
    casadi_assert(var.is_column() && var.is_valid_input(),
                          "DaeBuilder::nominal: Argument must be a symbolic vector");
    casadi_assert(var.nnz()==var.nnz(), "DaeBuilder::nominal: Dimension mismatch");
    std::vector<MX> prim = var.primitives();
    for (casadi_int i=0; i<prim.size(); ++i) {
      casadi_assert_dev(prim[i].nnz()==1);
      set_nominal(prim.at(i).name(), val.at(i));
    }
  }

  std::vector<double> DaeBuilder::attribute(getAtt f, const MX& var, bool normalized) const {
    casadi_assert(var.is_column() && var.is_valid_input(),
                          "DaeBuilder::attribute: Argument must be a symbolic vector");
    std::vector<double> ret(var.nnz());
    std::vector<MX> prim = var.primitives();
    for (casadi_int i=0; i<prim.size(); ++i) {
      casadi_assert_dev(prim[i].nnz()==1);
      ret[i] = (this->*f)(prim[i].name(), normalized);
    }
    return ret;
  }

  MX DaeBuilder::attribute(getAttS f, const MX& var) const {
    casadi_assert(var.is_column() && var.is_valid_input(),
                          "DaeBuilder::attribute: Argument must be a symbolic vector");
    MX ret = MX::zeros(var.sparsity());
    std::vector<MX> prim = var.primitives();
    for (casadi_int i=0; i<prim.size(); ++i) {
      casadi_assert_dev(prim[i].nnz()==1);
      ret.nz(i) = (this->*f)(prim[i].name());
    }
    return ret;
  }

  void DaeBuilder::set_attribute(setAtt f, const MX& var, const std::vector<double>& val,
                                 bool normalized) {
    casadi_assert(var.is_column() && var.is_valid_input(),
                          "DaeBuilder::set_attribute: Argument must be a symbolic vector");
    casadi_assert(var.nnz()==val.size(), "DaeBuilder::set_attribute: Dimension mismatch");
    std::vector<MX> prim = var.primitives();
    for (casadi_int i=0; i<prim.size(); ++i) {
      casadi_assert_dev(prim[i].nnz()==1);
      (this->*f)(prim[i].name(), val[i], normalized);
    }
  }

  void DaeBuilder::set_attribute(setAttS f, const MX& var, const MX& val) {
    casadi_assert(var.is_column() && var.is_valid_input(),
                          "DaeBuilder::set_attribute: Argument must be a symbolic vector");
    casadi_assert(var.sparsity()==val.sparsity(),
                          "DaeBuilder::set_attribute: Sparsity mismatch");
    std::vector<MX> prim = var.primitives();
    for (casadi_int i=0; i<prim.size(); ++i) {
      casadi_assert_dev(prim[i].nnz()==1);
      (this->*f)(var.nz(i).name(), val.nz(i));
    }
  }

  double DaeBuilder::min(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.min / v.nominal : v.min;
  }

  std::vector<double> DaeBuilder::min(const MX& var, bool normalized) const {
    return attribute(&DaeBuilder::min, var, normalized);
  }

  void DaeBuilder::set_min(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.min = normalized ? val*v.nominal : val;
  }

  void DaeBuilder::set_min(const MX& var, const std::vector<double>& val, bool normalized) {
    set_attribute(&DaeBuilder::set_min, var, val, normalized);
  }

  double DaeBuilder::max(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.max / v.nominal : v.max;
  }

  std::vector<double> DaeBuilder::max(const MX& var, bool normalized) const {
    return attribute(&DaeBuilder::max, var, normalized);
  }

  void DaeBuilder::set_max(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.max = normalized ? val*v.nominal : val;
  }

  void DaeBuilder::set_max(const MX& var, const std::vector<double>& val, bool normalized) {
    set_attribute(&DaeBuilder::set_max, var, val, normalized);
  }

  double DaeBuilder::guess(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.guess / v.nominal : v.guess;
  }

  std::vector<double> DaeBuilder::guess(const MX& var, bool normalized) const {
    return attribute(&DaeBuilder::guess, var, normalized);
  }

  void DaeBuilder::set_guess(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.guess = normalized ? val*v.nominal : val;
  }

  void DaeBuilder::set_guess(const MX& var, const std::vector<double>& val,
                                    bool normalized) {
    set_attribute(&DaeBuilder::set_guess, var, val, normalized);
  }

  double DaeBuilder::start(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.start / v.nominal : v.start;
  }

  std::vector<double> DaeBuilder::start(const MX& var, bool normalized) const {
    return attribute(&DaeBuilder::start, var, normalized);
  }

  void DaeBuilder::set_start(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.start = normalized ? val*v.nominal : val;
  }

  void DaeBuilder::set_start(const MX& var, const std::vector<double>& val, bool normalized) {
    set_attribute(&DaeBuilder::set_start, var, val, normalized);
  }

  double DaeBuilder::derivative_start(const std::string& name, bool normalized) const {
    const Variable& v = variable(name);
    return normalized ? v.derivative_start / v.nominal : v.derivative_start;
  }

  std::vector<double> DaeBuilder::derivative_start(const MX& var, bool normalized) const {
    return attribute(&DaeBuilder::derivative_start, var, normalized);
  }

  void DaeBuilder::set_derivative_start(const std::string& name, double val, bool normalized) {
    Variable& v = variable(name);
    v.derivative_start = normalized ? val*v.nominal : val;
  }

  void DaeBuilder::set_derivative_start(const MX& var, const std::vector<double>& val,
                                       bool normalized) {
    set_attribute(&DaeBuilder::set_derivative_start, var, val, normalized);
  }

  std::string DaeBuilder::name_in(DaeBuilderIn ind) {
    switch (ind) {
    case DAE_BUILDER_T: return "t";
    case DAE_BUILDER_C: return "c";
    case DAE_BUILDER_P: return "p";
    case DAE_BUILDER_V: return "v";
    case DAE_BUILDER_U: return "u";
    case DAE_BUILDER_X: return "x";
    case DAE_BUILDER_S: return "s";
    case DAE_BUILDER_SDOT: return "sdot";
    case DAE_BUILDER_Z: return "z";
    case DAE_BUILDER_Q: return "q";
    case DAE_BUILDER_Y: return "y";
    default: return "";
    }
  }

  DaeBuilder::DaeBuilderIn DaeBuilder::enum_in(const std::string& id) {
    if (id=="t") {
      return DAE_BUILDER_T;
    } else if (id=="c") {
      return DAE_BUILDER_C;
    } else if (id=="p") {
      return DAE_BUILDER_P;
    } else if (id=="v") {
      return DAE_BUILDER_V;
    } else if (id=="u") {
      return DAE_BUILDER_U;
    } else if (id=="x") {
      return DAE_BUILDER_X;
    } else if (id=="s") {
      return DAE_BUILDER_S;
    } else if (id=="sdot") {
      return DAE_BUILDER_SDOT;
    } else if (id=="z") {
      return DAE_BUILDER_Z;
    } else if (id=="q") {
      return DAE_BUILDER_Q;
    } else if (id=="y") {
      return DAE_BUILDER_Y;
    } else {
      return DAE_BUILDER_NUM_IN;
    }
  }

  std::vector<DaeBuilder::DaeBuilderIn>
  DaeBuilder::enum_in(const std::vector<std::string>& id) {
    std::vector<DaeBuilderIn> ret(id.size());
    for (casadi_int i=0; i<id.size(); ++i) {
      ret[i] = enum_in(id[i]);
    }
    return ret;
  }

  std::string DaeBuilder::name_out(DaeBuilderOut ind) {
    switch (ind) {
    case DAE_BUILDER_VDEF: return "vdef";
    case DAE_BUILDER_ODE: return "ode";
    case DAE_BUILDER_DAE: return "dae";
    case DAE_BUILDER_ALG: return "alg";
    case DAE_BUILDER_QUAD: return "quad";
    case DAE_BUILDER_YDEF: return "ydef";
    default: return "";
    }
  }

  DaeBuilder::DaeBuilderOut DaeBuilder::enum_out(const std::string& id) {
    if (id=="vdef") {
      return DAE_BUILDER_VDEF;
    } else if (id=="ode") {
      return DAE_BUILDER_ODE;
    } else if (id=="dae") {
      return DAE_BUILDER_DAE;
    } else if (id=="alg") {
      return DAE_BUILDER_ALG;
    } else if (id=="quad") {
      return DAE_BUILDER_QUAD;
    } else if (id=="ydef") {
      return DAE_BUILDER_YDEF;
    } else {
      return DAE_BUILDER_NUM_OUT;
    }
  }

  std::vector<DaeBuilder::DaeBuilderOut>
  DaeBuilder::enum_out(const std::vector<std::string>& id) {
    std::vector<DaeBuilderOut> ret(id.size());
    for (casadi_int i=0; i<id.size(); ++i) {
      ret[i] = enum_out(id[i]);
    }
    return ret;
  }

  std::string DaeBuilder::name_in() {
    std::stringstream ss;
    ss << "[";
    for (casadi_int i=0; i!=DAE_BUILDER_NUM_IN; ++i) {
      if (i!=0) ss << ",";
      ss << name_in(static_cast<DaeBuilderIn>(i));
    }
    ss << "]";
    return ss.str();
  }

  std::string DaeBuilder::name_out() {
    std::stringstream ss;
    ss << "[";
    for (casadi_int i=0; i!=DAE_BUILDER_NUM_OUT; ++i) {
      if (i!=0) ss << ",";
      ss << name_out(static_cast<DaeBuilderOut>(i));
    }
    ss << "]";
    return ss.str();
  }

  std::vector<MX> DaeBuilder::input(DaeBuilderIn ind) const {
    switch (ind) {
    case DAE_BUILDER_T: return std::vector<MX>(1, this->t);
    case DAE_BUILDER_C: return this->c;
    case DAE_BUILDER_P: return this->p;
    case DAE_BUILDER_V: return this->v;
    case DAE_BUILDER_U: return this->u;
    case DAE_BUILDER_X: return this->x;
    case DAE_BUILDER_S: return this->s;
    case DAE_BUILDER_SDOT: return this->sdot;
    case DAE_BUILDER_Z: return this->z;
    case DAE_BUILDER_Q: return this->q;
    case DAE_BUILDER_Y: return this->y;
    default: return std::vector<MX>();
    }
  }

  std::vector<MX> DaeBuilder::input(const std::vector<DaeBuilderIn>& ind) const {
    std::vector<MX> ret(ind.size());
    for (casadi_int i=0; i<ind.size(); ++i) {
      ret[i] = vertcat(input(ind[i]));
    }
    return ret;
  }

  std::vector<MX> DaeBuilder::output(DaeBuilderOut ind) const {
    switch (ind) {
    case DAE_BUILDER_VDEF: return this->vdef;
    case DAE_BUILDER_ODE: return this->ode;
    case DAE_BUILDER_DAE: return this->dae;
    case DAE_BUILDER_ALG: return this->alg;
    case DAE_BUILDER_QUAD: return this->quad;
    case DAE_BUILDER_YDEF: return this->ydef;
    default: return std::vector<MX>();
    }
  }

  std::vector<MX> DaeBuilder::output(const std::vector<DaeBuilderOut>& ind) const {
    std::vector<MX> ret(ind.size());
    for (casadi_int i=0; i<ind.size(); ++i) {
      ret[i] = vertcat(output(ind[i]));
    }
    return ret;
  }

  void DaeBuilder::add_lc(const std::string& name,
                        const std::vector<std::string>& f_out) {
    // Make sure object valid
    sanity_check();

    // Make sure name is valid
    casadi_assert(!name.empty(), "DaeBuilder::add_lc: \"name\" is empty");
    for (std::string::const_iterator i=name.begin(); i!=name.end(); ++i) {
      casadi_assert(isalnum(*i),
                            "DaeBuilder::add_lc: \"name\" must be alphanumeric");
    }

    // Consistency checks
    casadi_assert(!f_out.empty(), "DaeBuilder::add_lc: Linear combination is empty");
    std::vector<bool> in_use(DAE_BUILDER_NUM_OUT, false);
    for (casadi_int i=0; i<f_out.size(); ++i) {
      DaeBuilderOut oind = enum_out(f_out[i]);
      casadi_assert(oind!=DAE_BUILDER_NUM_OUT,
        "DaeBuilder::add_lc: No output expression " + f_out[i] + ". "
        "Valid expressions are " + name_out());
      casadi_assert(!in_use[oind],
        "DaeBuilder::add_lc: Duplicate expression " + f_out[i]);
      in_use[oind] = true;
    }

    std::vector<std::string>& ret1 = lc_[name];
    if (!ret1.empty()) casadi_warning("DaeBuilder::add_lc: Overwriting " << name);
    ret1 = f_out;
  }

  Function DaeBuilder::create(const std::string& fname,
      const std::vector<std::string>& s_in,
      const std::vector<std::string>& s_out, bool sx) const {
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
      return create(fname, s_in_mod, s_out_mod, sx);
    }
    // Check if dependent variables are given and needed
    bool elim_v;
    if (this->v.empty()) {
      // No dependent variables, no substitution needed
      elim_v = false;
    } else {
      // Dependent variables exists, eliminate unless v is given
      elim_v = true;
      for (const std::string& s : s_in) {
        if (s == "v") {
          // Dependent variables are given
          elim_v = false;
          break;
        }
      }
    }
    // Call factory
    return oracle(sx, elim_v).factory(fname, s_in, s_out, lc_);
  }

  Function DaeBuilder::add_fun(const Function& f) {
    casadi_assert(!has_fun(f.name()), "Function '" + f.name() + "' already exists");
    fun_.push_back(f);
    return f;
  }

  Function DaeBuilder::add_fun(const std::string& name,
                               const std::vector<std::string>& arg,
                               const std::vector<std::string>& res,
                               const Dict& opts) {
    casadi_assert(!has_fun(name), "Function '" + name + "' already exists");

    // Get inputs
    std::vector<MX> arg_ex, res_ex;
    for (auto&& s : arg) arg_ex.push_back(var(s));
    for (auto&& s : res) {
      // Find the binding expression FIXME(@jaeandersson)
      casadi_int v_ind;
      for (v_ind=0; v_ind<this->v.size(); ++v_ind) {
        if (s==this->v.at(v_ind).name()) {
          res_ex.push_back(this->vdef.at(v_ind));
          break;
        }
      }
      casadi_assert(v_ind<this->v.size(), "Cannot find dependent '" + s + "'");
    }
    Function ret(name, arg_ex, res_ex, arg, res, opts);
    return add_fun(ret);
  }

  Function DaeBuilder::add_fun(const std::string& name, const Importer& compiler,
                               const Dict& opts) {
    casadi_assert(!has_fun(name), "Function '" + name + "' already exists");
    return add_fun(external(name, compiler, opts));
  }

  bool DaeBuilder::has_fun(const std::string& name) const {
    for (const Function& f : fun_) {
      if (f.name()==name) return true;
    }
    return false;
  }

  Function DaeBuilder::fun(const std::string& name) const {
    casadi_assert(has_fun(name), "No such function: '" + name + "'");
    for (const Function& f : fun_) {
      if (f.name()==name) return f;
    }
    return Function();
  }

  void DaeBuilder::clear_cache() {
    for (bool elim_v : {false, true}) {
      if (!mx_oracle_[elim_v].is_null()) mx_oracle_[elim_v] = Function();
      if (!sx_oracle_[elim_v].is_null()) sx_oracle_[elim_v] = Function();
    }
  }

  const Function& DaeBuilder::oracle(bool sx, bool elim_v) const {
    // Create an MX oracle, if needed
    if (mx_oracle_[elim_v].is_null()) {
      // Oracle function inputs and outputs
      std::vector<MX> f_in, f_out, v;
      std::vector<std::string> f_in_name, f_out_name;
      // Do we need to substitute out v
      bool subst_v = false;
      // Collect all DAE input variables with at least one entry
      for (casadi_int i = 0; i != DAE_BUILDER_NUM_IN; ++i) {
        v = input(static_cast<DaeBuilderIn>(i));
        if (!v.empty()) {
          if (elim_v && i == DAE_BUILDER_V) {
            subst_v = true;
          } else {
            f_in.push_back(vertcat(v));
            f_in_name.push_back(name_in(static_cast<DaeBuilderIn>(i)));
          }
        }
      }
      // Collect all DAE output variables with at least one entry
      for (casadi_int i = 0; i != DAE_BUILDER_NUM_OUT; ++i) {
        v = output(static_cast<DaeBuilderOut>(i));
        if (!v.empty()) {
          f_out.push_back(vertcat(v));
          f_out_name.push_back(name_out(static_cast<DaeBuilderOut>(i)));
        }
      }
      // Eliminate v from inputs
      if (subst_v) {
        // Make a copy of dependent variable definitions to avoid modifying member variable
        std::vector<MX> vdef(this->vdef);
        // Perform in-place substitution
        substitute_inplace(this->v, vdef, f_out, false);
      }
      // Create oracle
      mx_oracle_[elim_v] = Function("mx_oracle", f_in, f_out, f_in_name, f_out_name);
    }
    // Return MX oracle, if requested
    if (!sx) return mx_oracle_[elim_v];
    // Create SX oracle, if needed
    if (sx_oracle_[elim_v].is_null()) {
      sx_oracle_[elim_v] = mx_oracle_[elim_v].expand("sx_oracle");
    }
    // Return SX oracle
    return sx_oracle_[elim_v];
  }

} // namespace casadi
