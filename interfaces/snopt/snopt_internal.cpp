/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010 by Joel Andersson, Moritz Diehl, K.U.Leuven. All rights reserved.
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

#include "snopt_internal.hpp"
#include "symbolic/stl_vector_tools.hpp"
#include "symbolic/matrix/matrix_tools.hpp"
#include "symbolic/mx/mx_tools.hpp"
#include "symbolic/matrix/sparsity_tools.hpp"
#include "symbolic/fx/mx_function.hpp"
#include <ctime>

#include <stdio.h>
#include <string.h>
#include "wsnopt.hpp"

using namespace std;

namespace CasADi{

  SnoptInternal::SnoptInternal(const FX& nlp) : NLPSolverInternal(nlp){
    addOption("detect_linear",OT_BOOLEAN, true, "Make an effort to treat linear constraints and linear variables specially.");
    
    // Monitors
    addOption("monitor",                  OT_STRINGVECTOR, GenericType(),  "", "eval_nlp|setup_nlp", true);
    
    // Snopt options
//    optionsmap_["_major_print_level"]     = std::pair<opt_type,std::string>(OT_INTEGER,"Major print level");
//    optionsmap_["_minor_print_level"]     = std::pair<opt_type,std::string>(OT_INTEGER,"Minor print level");
    optionsmap_["_verify_level"]          = std::pair<opt_type,std::string>(OT_INTEGER,"Verify level");
    optionsmap_["_iteration_limit"]       = std::pair<opt_type,std::string>(OT_INTEGER,"Iteration limit");

    optionsmap_["_feasibility_tolerance"] = std::pair<opt_type,std::string>(OT_REAL,   "Feasibility tolerance");
    optionsmap_["_optimality_tolerance"]  = std::pair<opt_type,std::string>(OT_REAL,   "Optimality tolerance");
    
    for (OptionsMap::const_iterator it=optionsmap_.begin();it!=optionsmap_.end();it++) {
      addOption(it->first,it->second.first, GenericType(), it->second.second);
    }
    
  }

  SnoptInternal::~SnoptInternal(){

  }
  
  SnoptInternal* SnoptInternal::clone() const{ 
    // Use default copy routine
    SnoptInternal* node = new SnoptInternal(*this); 
    
    return node;
  }

  void SnoptInternal::init(){

    detect_linear_ = getOption("detect_linear");
    
    // Call the init method of the base class
    NLPSolverInternal::init();


    // Get/generate required functions
    gradF();
    jacG();
    
    // Classify the decision variables into (2: nonlinear, 1: linear, 0:zero) according to the objective
    x_type_f_.resize(nx_);

    if (detect_linear_) {
      bvec_t* input_v_x =  get_bvec_t(gradF_->inputNoCheck(GRADF_X).data());
      bvec_t* input_v_p =  get_bvec_t(gradF_->inputNoCheck(GRADF_P).data());
      std::fill(input_v_x,input_v_x+nx_,bvec_t(1));
      std::fill(input_v_p,input_v_p+np_,bvec_t(0));
      bvec_t* output_v = get_bvec_t(gradF_->outputNoCheck().data());
      gradF_.spEvaluate(true);
      
      int k=0;
      for (int j=0;j<nx_;++j) {
        if (!gradF_.output().hasNZ(j,0)) {
          x_type_f_[j] = 0;
        } else {
          bool linear = true;
          x_type_f_[j] = output_v[k++]?  2 : 1;
        }
      }
    } else {
      std::fill(x_type_f_.begin(),x_type_f_.end(),2);
    }
    
    if(monitored("setup_nlp")){
      std::cout << "Variable classification (obj): " << x_type_f_ << std::endl;
    }
    // Classify the decision variables into (2: nonlinear, 1: linear, 0:zero) according to the constraints
    x_type_g_.resize(nx_);
    g_type_.resize(ng_);
    
    if (detect_linear_) {
      if (!jacG_.isNull()) {
        bvec_t* input_v_x =  get_bvec_t(jacG_->inputNoCheck(JACG_X).data());
        bvec_t* input_v_p =  get_bvec_t(jacG_->inputNoCheck(JACG_P).data());
        std::fill(input_v_x,input_v_x+nx_,bvec_t(1));
        std::fill(input_v_p,input_v_p+np_,bvec_t(0));
        bvec_t* output_v = get_bvec_t(jacG_->outputNoCheck().data());
        jacG_.spEvaluate(true);
        
        DMatrix out_trans = trans(jacG_.output());
        bvec_t* output_v_trans = get_bvec_t(out_trans.data());
        
        for (int j=0;j<nx_;++j) {
          if (jacG_.output().colind(j)==jacG_.output().colind(j+1)) {
            x_type_g_[j] = 0;
          } else {
            bool linear = true;
            for (int k=jacG_.output().colind(j);k<jacG_.output().colind(j+1);++k) {
              linear = linear && !output_v[k];
            }
            x_type_g_[j] = linear? 1 : 2;
          }
        }
        for (int j=0;j<ng_;++j) {
          if (out_trans.colind(j)==out_trans.colind(j+1)) {
            g_type_[j] = 0;
          } else {
            bool linear = true;
            for (int k=out_trans.colind(j);k<out_trans.colind(j+1);++k) {
              linear = linear && !output_v_trans[k];
            }
            g_type_[j] = linear? 1 : 2;
          }
        }
      } else {
        std::fill(x_type_g_.begin(),x_type_g_.end(),1);
        std::fill(g_type_.begin(),g_type_.end(),1);
      }
    } else {
      std::fill(x_type_g_.begin(),x_type_g_.end(),2);
      std::fill(g_type_.begin(),g_type_.end(),2);
    }
    if(monitored("setup_nlp")){
      std::cout << "Variable classification (con): " << x_type_g_ << std::endl;
      std::cout << "Constraint classification: " << g_type_ << std::endl;
    }
    std::vector<int> order_template;
    order_template.reserve(9);
    order_template.push_back(22);
    order_template.push_back(12);
    order_template.push_back(2);
    order_template.push_back(21);
    order_template.push_back(20);
    order_template.push_back(11);
    order_template.push_back(10);
    order_template.push_back(1);
    order_template.push_back(0);
    
    order_.resize(0);
    order_.reserve(nx_);
    
    std::vector<int> order_count;
    for (int p=0;p<order_template.size();++p) {
      for (int k=0;k<nx_;++k) {
        if (x_type_f_[k]*10+x_type_g_[k]==order_template[p]) {
          order_.push_back(k);
        }
      }
      order_count.push_back(order_.size());
    }
    if(monitored("setup_nlp")){
      for (int p=0;p<order_template.size();++p) {
        int start_k = (p>0 ?order_count[p-1]:0);
        std::cout << "Variables (" << order_template[p]/10 << "," << order_template[p]%10 << ") - " << order_count[p]-start_k << ":" << std::vector<int>(order_.begin()+start_k,order_.begin()+std::min(order_count[p],200+start_k)) << std::endl;
      }
    }
    
    nnJac_ = order_count[2];
    nnObj_ = order_count[4];
    
    order_g_.resize(0);
    order_g_.reserve(ng_);
    std::vector<int> order_g_count;
    for (int p=2;p>=0;--p) {
      for (int k=0;k<ng_;++k) {
        if (g_type_[k]==p) {
          order_g_.push_back(k);
        }
      }
      order_g_count.push_back(order_g_.size());
    }
    nnCon_ = order_g_count[0];
    
    // order : maps from sorted index to orginal index
    
    if(monitored("setup_nlp")){
      std::cout << "Variable order:" << order_ << std::endl;
      std::cout << "Constraint order:" << order_g_ << std::endl;
      std::cout << "nnJac:" << nnJac_ << std::endl;
      std::cout << "nnObj:" << nnObj_ << std::endl;
      std::cout << "nnCon:" << nnCon_ << std::endl;
    }

    IMatrix mapping_jacG  = IMatrix(0,nx_);
    IMatrix mapping_gradF = IMatrix(gradF_.output().sparsity(),range(-1,-1-gradF_.output().size(),-1));
    if (!jacG_.isNull()) {
      mapping_jacG = IMatrix(jacG_.output().sparsity(),range(1,jacG_.output().size()+1));
    }
    
    IMatrix d=trans(mapping_gradF(order_,Slice(0)));
    std::cout << "original_gradF indices: " << d << std::endl;
    for (int k=0;k<nnObj_;++k) {
      if (x_type_f_[order_[k]]==2) {
        d[k] = 0;
      }
    }
    d = sparse(d);
    if (d.size()==0) {
      gradF_row_ = false;
      A_structure_ = mapping_jacG(order_g_,order_);
      m_ = ng_;
    } else {
      gradF_row_ = true;
      A_structure_ = vertcat(mapping_jacG(order_g_,order_),d);
      m_ = ng_+1;
    }
    if (A_structure_.size()==0) {
      IMatrix dummyrow(1,nx_);
      dummyrow(0,0)=0;
      A_structure_ = vertcat(A_structure_,dummyrow);
      dummyrow_ = true;
      m_+=1;
    } else {
      dummyrow_ = false;
    }
    iObj_ = gradF_row_ ? m_ : 0;
        
    if(monitored("setup_nlp")){
      std::cout << "Objective gradient row presence: " << gradF_row_ << std::endl;
      std::cout << "Dummy row presence: " << dummyrow_ << std::endl;
      std::cout << "iObj: " << iObj_ << std::endl;
    }
   
    
    bl_.resize(nx_+m_);
    bu_.resize(nx_+m_);
    hs_.resize(nx_+m_);
    x_.resize(nx_+m_);
    pi_.resize(m_);
    rc_.resize(nx_+m_);
    A_data_.resize(A_structure_.size());

    int iPrint=0;
    int iSumm=0;
    
    // Manual says 500 is a bare minimum, which we will correct later
    int clen = 500; int rlen = 500; int ilen = 500;
    snopt_cw_.resize(clen*8,'a');
    snopt_iw_.resize(ilen,0);
    snopt_rw_.resize(rlen,0);
    
    snopt_init(&iPrint,&iSumm,getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen);
    
    int mincw = 0;
    int miniw = 0;
    int minrw = 0;
    int INFO = 0;
    int neA = A_structure_.size();
    int negCon = std::max(A_structure_(Slice(0,nnCon_),Slice(0,nnJac_)).size(),1);
    snopt_memb(&INFO, &m_,&nx_,&neA,&negCon,& nnCon_, &nnJac_, &nnObj_, &mincw, &miniw, &minrw, getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen);

    casadi_assert(INFO==104);
    
    snopt_cw_.resize(mincw*8,'a');
    snopt_iw_.resize(miniw,0);
    snopt_rw_.resize(minrw,0);

    iPrint = 9;
    iSumm = 6;
    snopt_init(&iPrint,&iSumm,getPtr(snopt_cw_),&mincw,getPtr(snopt_iw_),&miniw,getPtr(snopt_rw_),&minrw);

    for (OptionsMap::const_iterator it=optionsmap_.begin();it!=optionsmap_.end();it++) {
      int Error = 0;
      const std::string & snopt_name = it->second.second;
      int bufferlen = snopt_name.size();
      if (hasSetOption(it->first)) {
        std::cout << "setting option: " << snopt_name << std::endl;
        switch (it->second.first) {
          case OT_INTEGER: {
            int value = getOption(it->first);
            //snopt_seti(snopt_name.c_str(),&bufferlen,&value,&iPrint,&iSumm,&Error,getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen);
            snseti_(snopt_name.c_str(),&value,&iPrint,&iSumm,&Error,getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen,snopt_name.size(),snopt_cw_.size());
            }; break;
          case OT_REAL: {
          double value = getOption(it->first);
           snsetr_(snopt_name.c_str(),&value,&iPrint,&iSumm,&Error,getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen,snopt_name.size(),snopt_cw_.size());
/*            double value = getOption(it->first);
            std::string buffer = snopt_name;
            buffer.append(" ");
            std::stringstream s;
            s << value;
            buffer.append(s.str());
            int bufferlen2 = buffer.size();
            std::cout << snopt_name << "(" << bufferlen << ") :" << value << std::endl;
            std::cout << buffer << "(" << bufferlen2 << ")"  << std::endl;
            snopt_set(buffer.c_str(),&bufferlen2,&iPrint,&iSumm,&Error,getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen);
            */
            }; break;
          case OT_STRING: {
            std::string value = getOption(it->first);
            assert(value.size()<=8);
            value.append(8-value.size(),' ');
            std::string buffer = snopt_name;
            buffer.append(" = ");
            buffer.append(value);
            int bufferlen2 = buffer.size();
            std::cout << "buffer:" << buffer << std::endl;
            snopt_set(buffer.c_str(),&bufferlen2,&iPrint,&iSumm,&Error,getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen);
            }; break;
          default:
            casadi_error("Unkown type " << it->second.first);
        }
        casadi_assert_message(Error==0,"snopt error setting option \"" + snopt_name + "\"")
      } else {/**
        std::cout << "getting option: " << snopt_name << std::endl;
        switch (it->second.first) {
          case OT_INTEGER: {
            int value = 0;
            snopt_geti(snopt_name.c_str(),&bufferlen,&value,&Error,getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen);
            std::cout << "option value: " << value << std::endl << std::endl;
            setOption(it->first,value);
            }; break;
          case OT_REAL: {
            double value = 0;
            snopt_getr(snopt_name.c_str(),&bufferlen,&value,&Error,getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen);
            std::cout << "option value: " << value << std::endl << std::endl;
            setOption(it->first,value);
            }; break;
          case OT_STRING: {
            char value[8];
            snopt_getc(snopt_name.c_str(),&bufferlen,value,&Error,getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen);
            std::cout << "option value: " << value << std::endl << std::endl;
            setOption(it->first,std::string(value));
            }; break;
          default:
            casadi_error("Unkown type " << it->second.first);
        }
        casadi_assert_message(Error==0,"snopt error getting option \"" + snopt_name + "\"")*/
      }
    }
    
  }

  void SnoptInternal::reset(){

  }

  void SnoptInternal::setQPOptions() {

  }

  void SnoptInternal::passOptions() {

  }

  std::string SnoptInternal::formatStatus(int status) const {
    if (status_.find(status)==status_.end()) {
      std::stringstream ss;
      ss << "Unknown status: " << status;
      return ss.str();
    } else {
      return (*status_.find(status)).second;
    }
  }

  void SnoptInternal::evaluate(){
    log("SnoptInternal::evaluate");
     
    
    if (inputs_check_) checkInputs();
    
    checkInitialBounds();
    
    std::string start = "Cold";
    int lenstart = start.size();
    
    if (!jacG_.isNull()) {
      jacG_.setInput(input(NLP_SOLVER_X0),JACG_X);
      jacG_.setInput(input(NLP_SOLVER_P),JACG_P);
      jacG_.evaluate();
    }
    gradF_.setInput(input(NLP_SOLVER_X0),GRADF_X);
    gradF_.setInput(input(NLP_SOLVER_P),GRADF_P);

    gradF_.evaluate();
    for (int k=0;k<A_structure_.size();++k) {
      int i = A_structure_.data()[k];
      if (i==0) {
        A_data_[k] = 0;
      } else if (i>0) {
        A_data_[k] = jacG_.output().data()[i-1];
      } else {
        A_data_[k] = gradF_.output().data()[-i-1];
      }
    }

    // Obtain constraint offsets for linear constraints
    nlp_.setInput(0.0,NL_X);
    nlp_.setInput(input(NLP_SOLVER_P),NL_P);
    nlp_.evaluate();

    int n = nx_;
    int nea = A_structure_.size();
    int nNames=1;

    double ObjAdd = 0;
    std::string prob="  CasADi";
    
    std::vector<int> row(A_structure_.size());
    for (int k=0;k<A_structure_.size();++k) {
      row[k] = 1+A_structure_.row()[k];
    }
    
    std::vector<int> col(nx_+1);
    for (int k=0;k<nx_+1;++k) {
      col[k] = 1+A_structure_.colind()[k];
    }
    
    for (int k=0;k<nx_;++k) {
      int kk= order_[k];
      bl_[k] = input(NLP_SOLVER_LBX).data()[kk];
      bu_[k] = input(NLP_SOLVER_UBX).data()[kk];
      x_[k] = input(NLP_SOLVER_X0).data()[kk];
    }
    for (int k=0;k<ng_;++k) {
      int kk= order_g_[k];
      if (g_type_[kk]<2) {
        bl_[nx_+k] = input(NLP_SOLVER_LBG).data()[kk]-nlp_.output("g").data()[kk];
        bu_[nx_+k] = input(NLP_SOLVER_UBG).data()[kk]-nlp_.output("g").data()[kk];
      } else {
        bl_[nx_+k] = input(NLP_SOLVER_LBG).data()[kk];
        bu_[nx_+k] = input(NLP_SOLVER_UBG).data()[kk];
      }
      x_[nx_+k] = input(NLP_SOLVER_LAM_G0).data()[kk];
    }
    
    // Objective row should be unbounded
    if (bl_.size()>=nx_+ng_+1) {
      bl_[nx_+ng_] = -1e22;//-std::numeric_limits<double>::infinity();
      bu_[nx_+ng_] = 1e22;//std::numeric_limits<double>::infinity();
    }
    if (bl_.size()>=nx_+ng_+2) {
      bl_[nx_+ng_+1] = -1e22;//-std::numeric_limits<double>::infinity();
      bu_[nx_+ng_+1] = 1e22;//std::numeric_limits<double>::infinity();
    }
    
    int nS = 0;
    
    int clen = snopt_cw_.size()/8;
    int rlen = snopt_rw_.size();
    int ilen = snopt_iw_.size();

    int info=0;
    
    // Outputs
    int miniw=0,minrw=0,mincw=0;
    int nInf=0;
    double Obj= 0;
    double sInf=0;
    
    casadi_assert(m_>0);
    casadi_assert(n>0);
    casadi_assert(nea>0);
    casadi_assert(row.size()==nea);
    casadi_assert(hs_.size()==n+m_);
    casadi_assert(col.size()==n+1);
    casadi_assert(A_structure_.size()==nea);
    casadi_assert(bl_.size()==n+m_);
    casadi_assert(bu_.size()==n+m_);
    casadi_assert(pi_.size()==m_);
    casadi_assert(x_.size()==n+m_);
    casadi_assert(col.at(0)==1);
    casadi_assert(col.at(n)==nea+1);
    
    int iulen = 8;
    std::vector<int> iu(iulen);
//    memcpy(&(iu[0]), reinterpret_cast<IpoptInternal*>(iu), sizeof(IpoptInternal*));
    SnoptInternal* source = this;
    memcpy(&(iu[0]), &source, sizeof(SnoptInternal*));

    casadi_assert_message(!gradF_.isNull(),"blaasssshc");
    
    if(monitored("setup_nlp")){
      std::cout << "indA:" << row << std::endl;
      std::cout << "locA:" << col << std::endl;
      std::cout << "colA:" << A_data_ << std::endl;
      A_structure_.sparsity().spy();
      std::cout << "n:" << n << std::endl;
      std::cout << "m:" << m_ << std::endl;
      std::cout << "nea:" << nea << std::endl;
    }
    
    snopt_c(
      start.c_str(),&lenstart,&m_,&n,&nea,&nNames,&nnCon_,&nnObj_,&nnJac_,&iObj_,&ObjAdd,prob.c_str(),userfunPtr,
      
      getPtr(A_data_),getPtr(row),getPtr(col),getPtr(bl_),getPtr(bu_),
      
      0,
      
      // Initial values
      getPtr(hs_),getPtr(x_),getPtr(pi_),getPtr(rc_),
      
      // Outputs
      &info,&mincw,&miniw,&minrw,&nS,&nInf,&sInf,&Obj,
      
      // Working spaces for usrfun
      getPtr(snopt_cw_),&clen,getPtr(iu),&ilen,getPtr(snopt_rw_),&rlen,
      // Working spaces for SNOPT
      getPtr(snopt_cw_),&clen,getPtr(snopt_iw_),&ilen,getPtr(snopt_rw_),&rlen);

    for (int k=0;k<nx_;++k) {
      int kk= order_[k];
      output(NLP_SOLVER_X).data()[kk] = x_[k];
      output(NLP_SOLVER_LAM_X).data()[kk] = -rc_[k];
    }
    
    setOutput(Obj+ (gradF_row_? x_[nx_+ng_] : 0),NLP_SOLVER_F);
    for (int k=0;k<ng_;++k) {
      int kk= order_g_[k];
      output(NLP_SOLVER_LAM_G).data()[kk] = -rc_[nx_+k];
      output(NLP_SOLVER_G).data()[kk] =x_[nx_+k];
    }
    
    


  }

  void SnoptInternal::setOptionsFromFile(const std::string & file) {
    
  }
  
  void SnoptInternal::userfun(int& mode, int nnObj, int nnCon, int nnJac, int nnL, int neJac, double* x, double &fObj, double*gObj, double* fCon, double* gCon, int nState, char* cu, int lencu, int* iu, int leniu, double* ru, int lenru) {
    try {
      double time1 = clock();
      
      casadi_assert_message(nnCon_==nnCon,"Con " << nnCon_ << " <-> " << nnCon );
      casadi_assert_message(nnObj_==nnObj,"Obj " << nnObj_ << " <-> " << nnObj);
      casadi_assert_message(nnJac_==nnJac,"Jac " << nnJac_ << " <-> " << nnJac);

      gradF_.setInput(0.0,NL_X);
      for (int k=0;k<nnObj;++k) {
        if (x_type_f_[order_[k]]==2) {
          gradF_.input(NL_X)[order_[k]] = x[k];
        }
      }
      
      if(monitored("eval_nlp")){
        std::cout << "x (obj - sorted indices   - all elements present):" << std::vector<double>(x,x+nnObj) << std::endl;
        std::cout << "x (obj - original indices - linear elements zero):" << gradF_.input(NL_X) << std::endl;
      }
      
      gradF_.setInput(input(NLP_SOLVER_P),NL_P);
      gradF_.evaluate();
      for (int k=0;k<nnObj;++k) {
        if (x_type_f_[order_[k]]==2) {
         gObj[k] = gradF_.output().data()[order_[k]];
        }
      }
      
      fObj = gradF_.output(1).at(0);
      
      if(monitored("eval_nlp")){
        std::cout << "fObj:" << fObj << std::endl;
        std::cout << "gradF:" << gradF_.output() << std::endl;
        std::cout << "gObj:" << std::vector<double>(gObj,gObj+nnObj) << std::endl;
      }
      for (int k=0;k<A_structure_.size();++k) {
        int i = A_structure_.data()[k];
        if (i<0) {
          A_data_[k] = gradF_.output().data()[-i-1];
        }
      }
      if (!jacG_.isNull()) {

        // Pass the argument to the function
        jacG_.setInput(0.0,JACG_X);
        for (int k=0;k<nnJac ;++k) {
          jacG_.input(JACG_X)[order_[k]] = x[k];
        }
        if(monitored("eval_nlp")){
          std::cout << "x (con - sorted indices   - all elements present):" << std::vector<double>(x,x+nnJac) << std::endl;
          std::cout << "x (con - original indices - linear elements zero):" << jacG_.input(JACG_X) << std::endl;
        }
        jacG_.setInput(input(NLP_SOLVER_P),JACG_P);
      
        // Evaluate the function
        jacG_.evaluate();
        
        for (int k=0;k<A_structure_.size();++k) {
          int i = A_structure_.data()[k];
          if (i>0) {
            A_data_[k] = jacG_.output().data()[i-1];
          }
        }
        
        int kk=0;
        for (int j=0;j<nnJac;++j) {
          for (int k=A_structure_.colind(j);k<A_structure_.sparsity().colind(j+1);++k) {
            int row=A_structure_.row(k);
            if (row>=nnCon) break;
            gCon[kk++] = A_data_[k];
          }
        
        }
        casadi_assert(kk==0 || kk==neJac);
        if(monitored("eval_nlp")){
          std::cout << A_data_ << std::endl;
          std::cout << jacG_.output(GRADF_G) << std::endl;
        }
        DMatrix g=jacG_.output();
        for (int k=0;k<nnCon;++k) {
          fCon[k] = jacG_.output(GRADF_G).elem(order_g_[k],0);
        }
        if(monitored("eval_nlp")){
          std::cout << "fCon:" << std::vector<double>(fCon,fCon+nnCon) << std::endl;
          std::cout << "gCon:" << std::vector<double>(gCon,gCon+neJac) << std::endl;
        }
    }

    if (!callback_.isNull()) {
      for (int k=0;k<nx_;++k) {
        int kk= order_[k];
        output(NLP_SOLVER_X).data()[kk] = x_[k];
        //output(NLP_SOLVER_LAM_X).data()[kk] = -rc_[k];
      }
      
      //setOutput(Obj+ (gradF_row_? x_[nx_+ng_] : 0),NLP_SOLVER_F);
      for (int k=0;k<ng_;++k) {
        int kk= order_g_[k];
        //output(NLP_SOLVER_LAM_G).data()[kk] = -rc_[nx_+k];
        output(NLP_SOLVER_G).data()[kk] =x_[nx_+k];
      }
      
      mode = callback_(ref_,user_data_);
    }
    
   } catch (exception& ex){
     cerr << "eval_nlp failed: " << ex.what() << endl;
     mode = -1; // Reduce step size - we've got problems
     return;
   }
    
  }

  void SnoptInternal::userfunPtr(int * mode, int* nnObj, int * nnCon, int *nJac, int *nnL, int * neJac, double *x, double *fObj, double *gObj, double * fCon, double* gCon, int* nState, char* cu, int* lencu, int* iu, int* leniu, double* ru, int *lenru) {

    SnoptInternal* interface;//=reinterpret_cast<SnoptInternal*>(iu);
    memcpy(&interface,&(iu[0]), sizeof(SnoptInternal*));

    interface->userfun(*mode, *nnObj, *nnCon, *nJac, *nnL, *neJac, x,*fObj, gObj,fCon,gCon,  *nState,  cu,  *lencu,  iu, *leniu, ru, *lenru);
  }

} // namespace CasADi

