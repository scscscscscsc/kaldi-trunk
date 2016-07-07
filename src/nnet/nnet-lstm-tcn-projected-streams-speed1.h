// nnet/nnet-lstm-projected-streams.h

// Copyright 2014  Jiayu DU (Jerry), Wei Li

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.



#ifndef KALDI_NNET_NNET_LSTM_TCN_PROJECTED_STREAMS_SPEED_H_
#define KALDI_NNET_NNET_LSTM_TCN_PROJECTED_STREAMS_SPEED_H_

#include "nnet/nnet-component.h"
#include "nnet/nnet-utils.h"
#include "cudamatrix/cu-math.h"

/*************************************
 * x: input neuron
 * g: squashing neuron near input
 * i: Input gate
 * f: Forget gate
 * o: Output gate
 * c: memory Cell (CEC)
 * h: squashing neuron near output
 * m: output neuron of Memory block
 * r: recurrent projection neuron
 * y: output neuron of LSTMP
 *************************************/

namespace kaldi {
namespace nnet1 {

class LstmTcnProjectedStreamsSpeed : public UpdatableComponent {
 public:
  LstmTcnProjectedStreamsSpeed(int32 input_dim, int32 output_dim) :
    UpdatableComponent(input_dim, output_dim),
    input_dim_(input_dim),
    output_dim_(output_dim),
    ncell_(0),
    nrecur_(output_dim),
    nstream_(0),
    i1_(0),i2_(0),
    cj1_(0),cj2_(0),
    rj1_(0),rj2_(0),
    clip_gradient_(0.0)
    //, dropout_rate_(0.0)
  { }

  ~LstmTcnProjectedStreamsSpeed()
  { }

  Component* Copy() const { return new LstmTcnProjectedStreamsSpeed(*this); }
  ComponentType GetType() const { return scLstmTcnProjectedStreamsSpeed; }

  static void InitMatParam(CuMatrix<BaseFloat> &m, float scale) {
    m.SetRandUniform();  // uniform in [0, 1]
    m.Add(-0.5);         // uniform in [-0.5, 0.5]
    m.Scale(2 * scale);  // uniform in [-scale, +scale]
  }

  static void InitVecParam(CuVector<BaseFloat> &v, float scale) {
    Vector<BaseFloat> tmp(v.Dim());
    for (int i=0; i < tmp.Dim(); i++) {
      tmp(i) = (RandUniform() - 0.5) * 2 * scale;
    }
    v = tmp;
  }

  void InitData(std::istream &is) {
    // define options
    float param_scale = 0.02;
    // parse config
    std::string token;
    while (!is.eof()) {
      ReadToken(is, false, &token);
      if (token == "<CellDim>")
        ReadBasicType(is, false, &ncell_);     // ncell = i1 * i2
      else if (token == "<InputDim1>")
        ReadBasicType(is, false, &i1_);
      else if (token == "<InputDim2>")
        ReadBasicType(is, false, &i2_);
      else if (token == "<CellDim1>")
        ReadBasicType(is, false, &cj1_);
      else if (token == "<CellDim2>")
        ReadBasicType(is, false, &cj2_);
      else if (token == "<OutputDim1>")         // output = nrecur = rj1 * rj2
        ReadBasicType(is, false, &rj1_);       
      else if (token == "<OutputDim2>")
        ReadBasicType(is, false, &rj2_);
      else if (token == "<ClipGradient>")
        ReadBasicType(is, false, &clip_gradient_);
      //else if (token == "<DropoutRate>")
      //  ReadBasicType(is, false, &dropout_rate_);
      else if (token == "<ParamScale>")
        ReadBasicType(is, false, &param_scale);
      else KALDI_ERR << "Unknown token " << token << ", a typo in config?"
               << " (CellDim|ClipGradient|ParamScale)";
               //<< " (CellDim|ClipGradient|DropoutRate|ParamScale)";
      is >> std::ws;
    }
    
    // check dimension
    ncell_ = cj1_ * cj2_;
    //KALDI_LOG<<"output_dim_:"<<output_dim_<<"\t nrecur_:"<<nrecur_<<"\t rj1_:"<<rj1_<<"\t rj2_:"<<rj2_;
    //KALDI_LOG<<"input_dim_:"<<nrecur_<<"\t i1_:"<<rj1_<<"\t i2_:"<<rj2_;
    //KALDI_LOG<<"cj1_:"<<cj1_<<"\t cj2_:"<<cj2_;
    KALDI_ASSERT(nrecur_ == rj1_ * rj2_);
    KALDI_ASSERT(input_dim_ == i1_ * i2_);
    KALDI_ASSERT(output_dim_ == rj1_ * rj2_);

    // init weight and bias (Uniform)
    // input_dim_ = i1_ * i2_
    // ncell_ = cj1_ * cj2_
    // nrecur_ = rj1_ * rj2 = output_dim_
    // w_x: ncell_ * input_dim_
    w_g_x_mode_1_.Resize(cj1_, i1_, kUndefined);
    w_g_x_mode_2_.Resize(cj2_, i2_, kUndefined);
    w_i_x_mode_1_.Resize(cj1_, i1_, kUndefined);
    w_i_x_mode_2_.Resize(cj2_, i2_, kUndefined);
    w_f_x_mode_1_.Resize(cj1_, i1_, kUndefined);
    w_f_x_mode_2_.Resize(cj2_, i2_, kUndefined);
    w_o_x_mode_1_.Resize(cj1_, i1_, kUndefined);
    w_o_x_mode_2_.Resize(cj2_, i2_, kUndefined);
    // w_r: ncell_ * nrecur_
    w_g_r_mode_1_.Resize(cj1_, rj1_, kUndefined);
    w_g_r_mode_2_.Resize(cj2_, rj2_, kUndefined);
    w_i_r_mode_1_.Resize(cj1_, rj1_, kUndefined);
    w_i_r_mode_2_.Resize(cj2_, rj2_, kUndefined);
    w_f_r_mode_1_.Resize(cj1_, rj1_, kUndefined);
    w_f_r_mode_2_.Resize(cj2_, rj2_, kUndefined);
    w_o_r_mode_1_.Resize(cj1_, rj1_, kUndefined);
    w_o_r_mode_2_.Resize(cj2_, rj2_, kUndefined);

    // w_r_m_.Resize(nrecur_, ncell_, kUndefined);
    // w_r_m: nrecur_ * nrecur
    w_r_m_mode_1_.Resize(rj1_, cj1_, kUndefined);
    w_r_m_mode_2_.Resize(rj2_, cj2_, kUndefined);

    InitMatParam(w_g_x_mode_1_, param_scale);
    InitMatParam(w_g_x_mode_2_, param_scale);
    InitMatParam(w_i_x_mode_1_, param_scale);
    InitMatParam(w_i_x_mode_2_, param_scale);
    InitMatParam(w_f_x_mode_1_, param_scale);
    InitMatParam(w_f_x_mode_2_, param_scale);
    InitMatParam(w_o_x_mode_1_, param_scale);
    InitMatParam(w_o_x_mode_2_, param_scale);

    InitMatParam(w_g_r_mode_1_, param_scale);
    InitMatParam(w_g_r_mode_2_, param_scale);
    InitMatParam(w_i_r_mode_1_, param_scale);
    InitMatParam(w_i_r_mode_2_, param_scale);
    InitMatParam(w_f_r_mode_1_, param_scale);
    InitMatParam(w_f_r_mode_2_, param_scale);
    InitMatParam(w_o_r_mode_1_, param_scale);
    InitMatParam(w_o_r_mode_2_, param_scale);

    InitMatParam(w_r_m_mode_1_, param_scale);
    InitMatParam(w_r_m_mode_2_, param_scale);

    bias_.Resize(4*ncell_, kUndefined);
    peephole_i_c_.Resize(ncell_, kUndefined);
    peephole_f_c_.Resize(ncell_, kUndefined);
    peephole_o_c_.Resize(ncell_, kUndefined);

    InitVecParam(bias_, param_scale);
    InitVecParam(peephole_i_c_, param_scale);
    InitVecParam(peephole_f_c_, param_scale);
    InitVecParam(peephole_o_c_, param_scale);

    // init delta buffers
    w_g_x_corr_mode_1_.Resize(cj1_, i1_, kUndefined);
    w_g_x_corr_mode_2_.Resize(cj2_, i2_, kUndefined);
    w_i_x_corr_mode_1_.Resize(cj1_, i1_, kUndefined);
    w_i_x_corr_mode_2_.Resize(cj2_, i2_, kUndefined);
    w_f_x_corr_mode_1_.Resize(cj1_, i1_, kUndefined);
    w_f_x_corr_mode_2_.Resize(cj2_, i2_, kUndefined);
    w_o_x_corr_mode_1_.Resize(cj1_, i1_, kUndefined);
    w_o_x_corr_mode_2_.Resize(cj2_, i2_, kUndefined);

    w_g_r_corr_mode_1_.Resize(cj1_, rj1_, kUndefined);
    w_g_r_corr_mode_2_.Resize(cj2_, rj2_, kUndefined);
    w_i_r_corr_mode_1_.Resize(cj1_, rj1_, kUndefined);
    w_i_r_corr_mode_2_.Resize(cj2_, rj2_, kUndefined);
    w_f_r_corr_mode_1_.Resize(cj1_, rj1_, kUndefined);
    w_f_r_corr_mode_2_.Resize(cj2_, rj2_, kUndefined);
    w_o_r_corr_mode_1_.Resize(cj1_, rj1_, kUndefined);
    w_o_r_corr_mode_2_.Resize(cj2_, rj2_, kUndefined);

    bias_corr_.Resize(4*ncell_, kSetZero);

    peephole_i_c_corr_.Resize(ncell_, kSetZero);
    peephole_f_c_corr_.Resize(ncell_, kSetZero);
    peephole_o_c_corr_.Resize(ncell_, kSetZero);

    // w_r_m_corr_.Resize(nrecur_, ncell_, kSetZero);
    w_r_m_corr_mode_1_.Resize(rj1_, cj1_, kUndefined);
    w_r_m_corr_mode_2_.Resize(rj1_, cj2_, kUndefined);

    KALDI_ASSERT(clip_gradient_ >= 0.0);
  }

  void ReadData(std::istream &is, bool binary) {
    ExpectToken(is, binary, "<CellDim>");
    ReadBasicType(is, binary, &ncell_);
    ExpectToken(is, binary, "<InputDim1>");
    ReadBasicType(is, binary, &i1_);
    ExpectToken(is, binary, "<InputDim2>");
    ReadBasicType(is, binary, &i2_);
    ExpectToken(is, binary, "<CellDim1>");
    ReadBasicType(is, binary, &cj1_);
    ExpectToken(is, binary, "<CellDim2>");
    ReadBasicType(is, binary, &cj2_);
    ExpectToken(is, binary, "<OutputDim1>");
    ReadBasicType(is, binary, &rj1_);
    ExpectToken(is, binary, "<OutputDim2>");
    ReadBasicType(is, binary, &rj2_);

    ExpectToken(is, binary, "<ClipGradient>");
    ReadBasicType(is, binary, &clip_gradient_);
    //ExpectToken(is, binary, "<DropoutRate>");
    //ReadBasicType(is, binary, &dropout_rate_);

    // w_gifo_x_.Read(is, binary);
    // w_gifo_r_.Read(is, binary);
    w_g_x_mode_1_.Read(is, binary);
    w_g_x_mode_2_.Read(is, binary);
    w_i_x_mode_1_.Read(is, binary);
    w_i_x_mode_2_.Read(is, binary);
    w_f_x_mode_1_.Read(is, binary);
    w_f_x_mode_2_.Read(is, binary);
    w_o_x_mode_1_.Read(is, binary);
    w_o_x_mode_2_.Read(is, binary);

    w_g_r_mode_1_.Read(is, binary);
    w_g_r_mode_2_.Read(is, binary);
    w_i_r_mode_1_.Read(is, binary);
    w_i_r_mode_2_.Read(is, binary);
    w_f_r_mode_1_.Read(is, binary);
    w_f_r_mode_2_.Read(is, binary);
    w_o_r_mode_1_.Read(is, binary);
    w_o_r_mode_2_.Read(is, binary);

    bias_.Read(is, binary);

    peephole_i_c_.Read(is, binary);
    peephole_f_c_.Read(is, binary);
    peephole_o_c_.Read(is, binary);

    // w_r_m_.Read(is, binary);
    w_r_m_mode_1_.Read(is, binary);
    w_r_m_mode_2_.Read(is, binary);

    // init delta buffers
    // w_gifo_x_corr_.Resize(4*ncell_, input_dim_, kSetZero);
    // w_gifo_r_corr_.Resize(4*ncell_, nrecur_, kSetZero);
    w_g_x_corr_mode_1_.Resize(cj1_, i1_, kSetZero);
    w_g_x_corr_mode_2_.Resize(cj2_, i2_, kSetZero);
    w_i_x_corr_mode_1_.Resize(cj1_, i1_, kSetZero);
    w_i_x_corr_mode_2_.Resize(cj2_, i2_, kSetZero);
    w_f_x_corr_mode_1_.Resize(cj1_, i1_, kSetZero);
    w_f_x_corr_mode_2_.Resize(cj2_, i2_, kSetZero);
    w_o_x_corr_mode_1_.Resize(cj1_, i1_, kSetZero);
    w_o_x_corr_mode_2_.Resize(cj2_, i2_, kSetZero);

    w_g_r_corr_mode_1_.Resize(cj1_, rj1_, kSetZero);
    w_g_r_corr_mode_2_.Resize(cj2_, rj2_, kSetZero);
    w_i_r_corr_mode_1_.Resize(cj1_, rj1_, kSetZero);
    w_i_r_corr_mode_2_.Resize(cj2_, rj2_, kSetZero);
    w_f_r_corr_mode_1_.Resize(cj1_, rj1_, kSetZero);
    w_f_r_corr_mode_2_.Resize(cj2_, rj2_, kSetZero);
    w_o_r_corr_mode_1_.Resize(cj1_, rj1_, kSetZero);
    w_o_r_corr_mode_2_.Resize(cj2_, rj2_, kSetZero);

    bias_corr_.Resize(4*ncell_, kSetZero);

    peephole_i_c_corr_.Resize(ncell_, kSetZero);
    peephole_f_c_corr_.Resize(ncell_, kSetZero);
    peephole_o_c_corr_.Resize(ncell_, kSetZero);

    // w_r_m_corr_.Resize(nrecur_, ncell_, kSetZero);
    w_r_m_corr_mode_1_.Resize(rj1_, cj1_, kSetZero);
    w_r_m_corr_mode_2_.Resize(rj2_, cj1_, kSetZero);
  }

  void WriteData(std::ostream &os, bool binary) const {
    WriteToken(os, binary, "<CellDim>");
    WriteBasicType(os, binary, ncell_);
    WriteToken(os, binary, "<InputDim1>");
    WriteBasicType(os, binary, i1_);
    WriteToken(os, binary, "<InputDim2>");
    WriteBasicType(os, binary, i2_);
    WriteToken(os, binary, "<CellDim1>");
    WriteBasicType(os, binary, cj1_);
    WriteToken(os, binary, "<CellDim2>");
    WriteBasicType(os, binary, cj2_);
    WriteToken(os, binary, "<OutputDim1>");
    WriteBasicType(os, binary, rj1_);
    WriteToken(os, binary, "<OutputDim2>");
    WriteBasicType(os, binary, rj2_);

    WriteToken(os, binary, "<ClipGradient>");
    WriteBasicType(os, binary, clip_gradient_);
    //WriteToken(os, binary, "<DropoutRate>");
    //WriteBasicType(os, binary, dropout_rate_);

    // w_gifo_x_.Write(os, binary);
    // w_gifo_r_.Write(os, binary);
    w_g_x_mode_1_.Write(os, binary);
    w_g_x_mode_2_.Write(os, binary);
    w_i_x_mode_1_.Write(os, binary);
    w_i_x_mode_2_.Write(os, binary);
    w_f_x_mode_1_.Write(os, binary);
    w_f_x_mode_2_.Write(os, binary);
    w_o_x_mode_1_.Write(os, binary);
    w_o_x_mode_2_.Write(os, binary);

    w_g_r_mode_1_.Write(os, binary);
    w_g_r_mode_2_.Write(os, binary);
    w_i_r_mode_1_.Write(os, binary);
    w_i_r_mode_2_.Write(os, binary);
    w_f_r_mode_1_.Write(os, binary);
    w_f_r_mode_2_.Write(os, binary);
    w_o_r_mode_1_.Write(os, binary);
    w_o_r_mode_2_.Write(os, binary);

    bias_.Write(os, binary);

    peephole_i_c_.Write(os, binary);
    peephole_f_c_.Write(os, binary);
    peephole_o_c_.Write(os, binary);

    // w_r_m_.Write(os, binary);
    w_r_m_mode_1_.Write(os, binary);
    w_r_m_mode_2_.Write(os, binary);
  }

  int32 NumParams() const {
    return ( // w_gifo_x_.NumRows() * w_gifo_x_.NumCols() +
         // w_gifo_r_.NumRows() * w_gifo_r_.NumCols() +
         w_g_x_mode_1_.NumRows() * w_g_x_mode_1_.NumCols() + 
         w_i_x_mode_1_.NumRows() * w_i_x_mode_1_.NumCols() +
         w_f_x_mode_1_.NumRows() * w_f_x_mode_1_.NumCols() +
         w_o_x_mode_1_.NumRows() * w_o_x_mode_1_.NumCols() +
         w_g_x_mode_2_.NumRows() * w_g_x_mode_2_.NumCols() + 
         w_i_x_mode_2_.NumRows() * w_i_x_mode_2_.NumCols() +
         w_f_x_mode_2_.NumRows() * w_f_x_mode_2_.NumCols() +
         w_o_x_mode_2_.NumRows() * w_o_x_mode_2_.NumCols() +
         w_g_r_mode_1_.NumRows() * w_g_r_mode_1_.NumCols() + 
         w_i_r_mode_1_.NumRows() * w_i_r_mode_1_.NumCols() +
         w_f_r_mode_1_.NumRows() * w_f_r_mode_1_.NumCols() +
         w_o_r_mode_1_.NumRows() * w_o_r_mode_1_.NumCols() +
         w_g_r_mode_2_.NumRows() * w_g_r_mode_2_.NumCols() + 
         w_i_r_mode_2_.NumRows() * w_i_r_mode_2_.NumCols() +
         w_f_r_mode_2_.NumRows() * w_f_r_mode_2_.NumCols() +
         w_o_r_mode_2_.NumRows() * w_o_r_mode_2_.NumCols() +
         bias_.Dim() +
         peephole_i_c_.Dim() +
         peephole_f_c_.Dim() +
         peephole_o_c_.Dim() +
         // w_r_m_.NumRows() * w_r_m_.NumCols() );
         w_r_m_mode_1_.NumRows() * w_r_m_mode_1_.NumCols() +
         w_r_m_mode_2_.NumRows() * w_r_m_mode_2_.NumCols() );
  }

  void GetParams(Vector<BaseFloat>* wei_copy) const {
    wei_copy->Resize(NumParams());

    int32 offset, len;

    offset = 0;  len = w_g_x_mode_1_.NumRows() * w_g_x_mode_1_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_g_x_mode_1_);

    offset += len; len = w_g_x_mode_2_.NumRows() * w_g_x_mode_2_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_g_x_mode_2_);
    
    offset += len; len = w_i_x_mode_1_.NumRows() * w_i_x_mode_1_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_i_x_mode_1_);

    offset += len; len = w_i_x_mode_2_.NumRows() * w_i_x_mode_2_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_i_x_mode_2_);

    offset += len; len = w_f_x_mode_1_.NumRows() * w_f_x_mode_1_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_f_x_mode_1_);

    offset += len; len = w_f_x_mode_2_.NumRows() * w_f_x_mode_2_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_f_x_mode_2_);

    offset += len; len = w_o_x_mode_1_.NumRows() * w_o_x_mode_1_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_o_x_mode_1_);

    offset += len; len = w_o_x_mode_2_.NumRows() * w_o_x_mode_2_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_o_x_mode_2_);

    offset += len; len = w_g_r_mode_1_.NumRows() * w_g_r_mode_1_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_g_r_mode_1_);

    offset += len; len = w_g_r_mode_2_.NumRows() * w_g_r_mode_2_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_g_r_mode_2_);
    
    offset += len; len = w_i_r_mode_1_.NumRows() * w_i_r_mode_1_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_i_r_mode_1_);

    offset += len; len = w_i_r_mode_2_.NumRows() * w_i_r_mode_2_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_i_r_mode_2_);

    offset += len; len = w_f_r_mode_1_.NumRows() * w_f_r_mode_1_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_f_r_mode_1_);

    offset += len; len = w_f_r_mode_2_.NumRows() * w_f_r_mode_2_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_f_r_mode_2_);

    offset += len; len = w_o_r_mode_1_.NumRows() * w_o_r_mode_1_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_o_r_mode_1_);

    offset += len; len = w_o_r_mode_2_.NumRows() * w_o_r_mode_2_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_o_r_mode_2_);

    offset += len; len = bias_.Dim();
    wei_copy->Range(offset, len).CopyFromVec(bias_);

    offset += len; len = peephole_i_c_.Dim();
    wei_copy->Range(offset, len).CopyFromVec(peephole_i_c_);

    offset += len; len = peephole_f_c_.Dim();
    wei_copy->Range(offset, len).CopyFromVec(peephole_f_c_);

    offset += len; len = peephole_o_c_.Dim();
    wei_copy->Range(offset, len).CopyFromVec(peephole_o_c_);

    // offset += len; len = w_r_m_.NumRows() * w_r_m_.NumCols();
    // wei_copy->Range(offset, len).CopyRowsFromMat(w_r_m_);
    offset += len; len = w_r_m_mode_1_.NumRows() * w_r_m_mode_1_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_r_m_mode_1_);

    offset += len; len = w_r_m_mode_2_.NumRows() * w_r_m_mode_2_.NumCols();
    wei_copy->Range(offset, len).CopyRowsFromMat(w_r_m_mode_2_);

    return;
  }

  std::string Info() const {
    return std::string("  ") +
      "\n  w_g_x_mode_1_  "   + MomentStatistics(w_g_x_mode_1_) +
      "\n  w_g_x_mode_2_  "   + MomentStatistics(w_g_x_mode_2_) +
      "\n  w_i_x_mode_1_  "   + MomentStatistics(w_i_x_mode_1_) +
      "\n  w_i_x_mode_2_  "   + MomentStatistics(w_i_x_mode_2_) +
      "\n  w_f_x_mode_1_  "   + MomentStatistics(w_f_x_mode_1_) +
      "\n  w_f_x_mode_2_  "   + MomentStatistics(w_f_x_mode_2_) +
      "\n  w_o_x_mode_1_  "   + MomentStatistics(w_o_x_mode_1_) +
      "\n  w_o_x_mode_2_  "   + MomentStatistics(w_o_x_mode_2_) +
      "\n  w_g_r_mode_1_  "   + MomentStatistics(w_g_r_mode_1_) +
      "\n  w_g_r_mode_2_  "   + MomentStatistics(w_g_r_mode_2_) +
      "\n  w_i_r_mode_1_  "   + MomentStatistics(w_i_r_mode_1_) +
      "\n  w_i_r_mode_2_  "   + MomentStatistics(w_i_r_mode_2_) +
      "\n  w_f_r_mode_1_  "   + MomentStatistics(w_f_r_mode_1_) +
      "\n  w_f_r_mode_2_  "   + MomentStatistics(w_f_r_mode_2_) +
      "\n  w_o_r_mode_1_  "   + MomentStatistics(w_o_r_mode_1_) +
      "\n  w_o_r_mode_2_  "   + MomentStatistics(w_o_r_mode_2_) +
      "\n  bias_  "     + MomentStatistics(bias_) +
      "\n  peephole_i_c_  " + MomentStatistics(peephole_i_c_) +
      "\n  peephole_f_c_  " + MomentStatistics(peephole_f_c_) +
      "\n  peephole_o_c_  " + MomentStatistics(peephole_o_c_) +
      "\n  w_r_m_mode_1_  "    + MomentStatistics(w_r_m_mode_1_) +
      "\n  w_r_m_mode_2_  "    + MomentStatistics(w_r_m_mode_2_);
      //"\n  w_r_m_  "    + MomentStatistics(w_r_m_);
  }

  std::string InfoGradient() const {
    // disassemble forward-propagation buffer into different neurons,
    const CuSubMatrix<BaseFloat> YG(propagate_buf_.ColRange(0*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> YI(propagate_buf_.ColRange(1*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> YF(propagate_buf_.ColRange(2*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> YO(propagate_buf_.ColRange(3*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> YC(propagate_buf_.ColRange(4*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> YH(propagate_buf_.ColRange(5*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> YM(propagate_buf_.ColRange(6*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> YR(propagate_buf_.ColRange(7*ncell_, nrecur_));

    // disassemble backpropagate buffer into different neurons,
    const CuSubMatrix<BaseFloat> DG(backpropagate_buf_.ColRange(0*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> DI(backpropagate_buf_.ColRange(1*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> DF(backpropagate_buf_.ColRange(2*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> DO(backpropagate_buf_.ColRange(3*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> DC(backpropagate_buf_.ColRange(4*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> DH(backpropagate_buf_.ColRange(5*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> DM(backpropagate_buf_.ColRange(6*ncell_, ncell_));
    const CuSubMatrix<BaseFloat> DR(backpropagate_buf_.ColRange(7*ncell_, nrecur_));

    return std::string("  ") +
      "\n  Gradients:" +
      "\n  w_g_x_corr_mode_1_  " + MomentStatistics(w_g_x_corr_mode_1_) +
      "\n  w_g_x_corr_mode_2_  " + MomentStatistics(w_g_x_corr_mode_2_) +
      "\n  w_i_x_corr_mode_1_  " + MomentStatistics(w_i_x_corr_mode_1_) +
      "\n  w_i_x_corr_mode_2_  " + MomentStatistics(w_i_x_corr_mode_2_) +
      "\n  w_f_x_corr_mode_1_  " + MomentStatistics(w_f_x_corr_mode_1_) +
      "\n  w_f_x_corr_mode_2_  " + MomentStatistics(w_f_x_corr_mode_2_) +
      "\n  w_o_x_corr_mode_1_  " + MomentStatistics(w_o_x_corr_mode_1_) +
      "\n  w_o_x_corr_mode_2_  " + MomentStatistics(w_o_x_corr_mode_2_) +
      "\n  w_g_r_corr_mode_1_  " + MomentStatistics(w_g_r_corr_mode_1_) +
      "\n  w_g_r_corr_mode_2_  " + MomentStatistics(w_g_r_corr_mode_2_) +
      "\n  w_i_r_corr_mode_1_  " + MomentStatistics(w_i_r_corr_mode_1_) +
      "\n  w_i_r_corr_mode_2_  " + MomentStatistics(w_i_r_corr_mode_2_) +
      "\n  w_f_r_corr_mode_1_  " + MomentStatistics(w_f_r_corr_mode_1_) +
      "\n  w_f_r_corr_mode_2_  " + MomentStatistics(w_f_r_corr_mode_2_) +
      "\n  w_o_r_corr_mode_1_  " + MomentStatistics(w_o_r_corr_mode_1_) +
      "\n  w_o_r_corr_mode_2_  " + MomentStatistics(w_o_r_corr_mode_2_) +
      "\n  bias_corr_  "     + MomentStatistics(bias_corr_) +
      "\n  peephole_i_c_corr_  " + MomentStatistics(peephole_i_c_corr_) +
      "\n  peephole_f_c_corr_  " + MomentStatistics(peephole_f_c_corr_) +
      "\n  peephole_o_c_corr_  " + MomentStatistics(peephole_o_c_corr_) +
      "\n  w_r_m_corr_mode_1_  " + MomentStatistics(w_r_m_corr_mode_1_) +
      "\n  w_r_m_corr_mode_2_  " + MomentStatistics(w_r_m_corr_mode_2_) +
      "\n  Forward-pass:" +
      "\n  YG  " + MomentStatistics(YG) +
      "\n  YI  " + MomentStatistics(YI) +
      "\n  YF  " + MomentStatistics(YF) +
      "\n  YC  " + MomentStatistics(YC) +
      "\n  YH  " + MomentStatistics(YH) +
      "\n  YO  " + MomentStatistics(YO) +
      "\n  YM  " + MomentStatistics(YM) +
      "\n  YR  " + MomentStatistics(YR) +
      "\n  Backward-pass:" +
      "\n  DG  " + MomentStatistics(DG) +
      "\n  DI  " + MomentStatistics(DI) +
      "\n  DF  " + MomentStatistics(DF) +
      "\n  DC  " + MomentStatistics(DC) +
      "\n  DH  " + MomentStatistics(DH) +
      "\n  DO  " + MomentStatistics(DO) +
      "\n  DM  " + MomentStatistics(DM) +
      "\n  DR  " + MomentStatistics(DR);
  }

  void ResetLstmStreams(const std::vector<int32> &stream_reset_flag) {
    // allocate prev_nnet_state_ if not done yet,
    if (nstream_ == 0) {
      // Karel: we just got number of streams! (before the 1st batch comes)
      nstream_ = stream_reset_flag.size();
      prev_nnet_state_.Resize(nstream_, 7*ncell_ + 1*nrecur_, kSetZero);
      KALDI_LOG << "Running training with " << nstream_ << " streams.";
    }
    // reset flag: 1 - reset stream network state
    KALDI_ASSERT(prev_nnet_state_.NumRows() == stream_reset_flag.size());
    for (int s = 0; s < stream_reset_flag.size(); s++) {
      if (stream_reset_flag[s] == 1) {
        prev_nnet_state_.Row(s).SetZero();
      }
    }
  }

  void TuckerFeedForward(const CuMatrixBase<BaseFloat> &wei1, const CuMatrixBase<BaseFloat> &wei2,
                         const CuMatrixBase<BaseFloat> &in, CuMatrixBase<BaseFloat>* out)
  {
    //KALDI_LOG<<"wei1:"<<"("<<wei1.NumRows()<<","<<wei1.NumCols()<<")";
    //KALDI_LOG<<"wei2:"<<"("<<wei2.NumRows()<<","<<wei2.NumCols()<<")";
    //KALDI_LOG<<"in:("<<in.NumRows()<<","<<in.NumCols()<<")";
    //KALDI_LOG<<"out"<<"("<<out->NumRows()<<","<<out->NumCols()<<")";

    int32 ib = out->NumRows();
    int32 j1 = wei1.NumRows(), i1 = wei1.NumCols();
    int32 j2 = wei2.NumRows(), i2 = wei2.NumCols();
    // give initial size
    CuMatrix<BaseFloat> output(ib, j1 * j2);
    CuMatrix<BaseFloat> temp(ib, j1 * i2);
    // reshape to std::vector
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_input;
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_output;
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_temp;
    // w1_vec and w2_vec used to parallel
    std::vector< CuSubMatrix<BaseFloat>* > w1_vec;
    std::vector< CuSubMatrix<BaseFloat>* > w2_vec;
    CuSubMatrix<BaseFloat>* w1 = new CuSubMatrix<BaseFloat>(wei1,0,j1,0,i1);
    CuSubMatrix<BaseFloat>* w2 = new CuSubMatrix<BaseFloat>(wei2,0,j2,0,i2);
    // initial some vectors
    for(int32 i = 0; i < ib; i++)
    {
      CuSubMatrix<BaseFloat> *temp_reshaped_input = new CuSubMatrix<BaseFloat>(in,i,i1,i2);
      CuSubMatrix<BaseFloat> *temp_reshaped_temp = new CuSubMatrix<BaseFloat>(temp,i,j1,i2);
      CuSubMatrix<BaseFloat> *temp_reshaped_output = new CuSubMatrix<BaseFloat>(output,i,j1,j2);
      w1_vec.push_back(w1);
      w2_vec.push_back(w2);
      reshaped_input.push_back(temp_reshaped_input);
      reshaped_temp.push_back(temp_reshaped_temp);
      reshaped_output.push_back(temp_reshaped_output);
    }
    AddMatMatBatched<BaseFloat>(1.0,reshaped_temp,w1_vec,kNoTrans,reshaped_input,kNoTrans,0.0);
    AddMatMatBatched<BaseFloat>(1.0,reshaped_output,reshaped_temp,kNoTrans,w2_vec,kTrans,0.0);
    out->CopyFromMat(output);
    // delete pointers
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_input.begin(); i!=reshaped_input.end(); ++i)
      delete *i;
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_output.begin(); i!=reshaped_output.end(); ++i)
      delete *i;
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_temp.begin(); i!=reshaped_temp.end(); ++i)
      delete *i;
    delete w1; delete w2;
  }
  
  // note: out_diff should be tensor type 1, this can save some memory and computation
  void TuckerBackProp(const CuMatrixBase<BaseFloat> &wei1, const CuMatrixBase<BaseFloat> &wei2,
                      const CuMatrixBase<BaseFloat> &out_diff, CuMatrixBase<BaseFloat> *in_diff)
  {
    int32 ib = out_diff.NumRows();
    int32 j1 = wei1.NumRows(), i1 = wei1.NumCols();
    int32 j2 = wei2.NumRows(), i2 = wei2.NumCols();
    // given initial size
    CuMatrix<BaseFloat> temp(ib, i1 * j2);
    CuMatrix<BaseFloat> input_diff(ib, i1 * i2);
    // reshape to std::vector
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_temp;
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_input_diff;
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_output_diff;
    // w1_vec and w2_vec used to parallel
    std::vector< CuSubMatrix<BaseFloat>* > w1_vec;
    std::vector< CuSubMatrix<BaseFloat>* > w2_vec;
    CuSubMatrix<BaseFloat>* w1 = new CuSubMatrix<BaseFloat>(wei1,0,j1,0,i1);
    CuSubMatrix<BaseFloat>* w2 = new CuSubMatrix<BaseFloat>(wei2,0,j2,0,i2);
    // initial some vectors
    for(int32 i = 0; i < ib; i++)
    {
      CuSubMatrix<BaseFloat> *temp_reshaped_temp = new CuSubMatrix<BaseFloat>(temp,i,i1,j2);               //i1 * j2
      CuSubMatrix<BaseFloat> *temp_reshaped_output_diff = new CuSubMatrix<BaseFloat>(out_diff,i,j1,j2);    //j1 * j2
      CuSubMatrix<BaseFloat> *temp_reshaped_input_diff = new CuSubMatrix<BaseFloat>(input_diff,i,i1,i2);   //i1 * i2
      w1_vec.push_back(w1);
      w2_vec.push_back(w2);
      reshaped_temp.push_back(temp_reshaped_temp);
      reshaped_output_diff.push_back(temp_reshaped_output_diff);
      reshaped_input_diff.push_back(temp_reshaped_input_diff);
    }
    AddMatMatBatched<BaseFloat>(1.0,reshaped_temp,w1_vec,kTrans,reshaped_output_diff,kNoTrans,0.0);
    AddMatMatBatched<BaseFloat>(1.0,reshaped_input_diff,reshaped_temp,kNoTrans,w2_vec,kNoTrans,0.0);
    
    in_diff->CopyFromMat(input_diff);
    // delete pointers
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_temp.begin(); i!=reshaped_temp.end(); ++i)
      delete *i;
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_input_diff.begin(); i!=reshaped_input_diff.end(); ++i)
      delete *i;
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_output_diff.begin(); i!=reshaped_output_diff.end(); ++i)
      delete *i;
    delete w1; delete w2;
  }

    // note: in and diff should be tensor type 0, it's easy to compute
  void TuckerCalcGrad(const CuMatrixBase<BaseFloat> &wei1, const CuMatrixBase<BaseFloat> &wei2,
                      const CuMatrixBase<BaseFloat> &in, const CuMatrixBase<BaseFloat> &diff,
                      CuMatrixBase<BaseFloat> *gw1, CuMatrixBase<BaseFloat> *gw2,
                      const BaseFloat mmt)
  {
    int32 ib = in.NumRows();
    int32 j1 = wei1.NumRows(), i1 = wei1.NumCols();
    int32 j2 = wei2.NumRows(), i2 = wei2.NumCols();
    // give initial size
    //gw1->Resize(j1, i1);
    //gw2->Resize(j2, i2);
    CuMatrix<BaseFloat> input(ib, j1 * i2);
    CuMatrix<BaseFloat> dif(ib, j1 * j2);
    CuMatrix<BaseFloat> temp_w1(ib, j1 * i2);
    CuMatrix<BaseFloat> temp_w2(ib, j2 * i1);
    CuMatrix<BaseFloat> w1_grad_batch(ib, j1 * i1);
    CuMatrix<BaseFloat> w2_grad_batch(ib, j2 * i2);
    // reshape to std::vector
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_input;
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_diff;
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_temp_w1;
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_temp_w2;
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_w1_grad_batch;
    std::vector< CuSubMatrix<BaseFloat>* > reshaped_w2_grad_batch;
    // w1_vec and w2_vec used to parallel
    std::vector< CuSubMatrix<BaseFloat>* > w1_vec;
    std::vector< CuSubMatrix<BaseFloat>* > w2_vec;
    CuSubMatrix<BaseFloat>* w1 = new CuSubMatrix<BaseFloat>(wei1,0,j1,0,i1);
    CuSubMatrix<BaseFloat>* w2 = new CuSubMatrix<BaseFloat>(wei2,0,j2,0,i2);
    // initial some vectors
    for(int32 i = 0; i < ib; i++)
    {
      CuSubMatrix<BaseFloat> *temp_reshaped_input = new CuSubMatrix<BaseFloat>(in,i,i1,i2);
      CuSubMatrix<BaseFloat> *temp_reshaped_diff = new CuSubMatrix<BaseFloat>(diff,i,j1,j2);
      CuSubMatrix<BaseFloat> *temp_reshaped_temp_w1 = new CuSubMatrix<BaseFloat>(temp_w1,i,j1,i2);                  //j1 * i2
      CuSubMatrix<BaseFloat> *temp_reshaped_temp_w2 = new CuSubMatrix<BaseFloat>(temp_w2,i,j2,i1);                  //j2 * i1
      CuSubMatrix<BaseFloat> *temp_w1_grad_batch = new CuSubMatrix<BaseFloat>(w1_grad_batch,i,j1,i1);               //j1 * i1
      CuSubMatrix<BaseFloat> *temp_w2_grad_batch = new CuSubMatrix<BaseFloat>(w2_grad_batch,i,j2,i2);               //j2 * i2
      w1_vec.push_back(w1);
      w2_vec.push_back(w2);
      reshaped_input.push_back(temp_reshaped_input);
      reshaped_diff.push_back(temp_reshaped_diff);
      reshaped_temp_w1.push_back(temp_reshaped_temp_w1);
      reshaped_temp_w2.push_back(temp_reshaped_temp_w2);
      reshaped_w1_grad_batch.push_back(temp_w1_grad_batch);
      reshaped_w2_grad_batch.push_back(temp_w2_grad_batch);
    }
    // compute gradient
    // we have w1 and w2 gradient
    // compute w1 grad
    AddMatMatBatched<BaseFloat>(1.0,reshaped_temp_w1,reshaped_diff,kNoTrans,w2_vec,kNoTrans,0.0);
    AddMatMatBatched<BaseFloat>(1.0,reshaped_w1_grad_batch,reshaped_temp_w1,kNoTrans,reshaped_input,kTrans,0.0);
    CuVector<BaseFloat> w1_grad_vec_temp(j1 * i1);
    w1_grad_vec_temp.AddRowSumMat(1.0,w1_grad_batch);
    gw1->CopyRowsFromVec(w1_grad_vec_temp);
    gw1->AddMat(mmt,*gw1);
    // compute w2 grad
    AddMatMatBatched<BaseFloat>(1.0,reshaped_temp_w2,reshaped_diff,kTrans,w1_vec,kNoTrans,0.0);
    AddMatMatBatched<BaseFloat>(1.0,reshaped_w2_grad_batch,reshaped_temp_w2,kNoTrans,reshaped_input,kNoTrans,0.0);
    CuVector<BaseFloat> w2_grad_vec_temp(j2 * i2);
    w2_grad_vec_temp.AddRowSumMat(1.0,w2_grad_batch);
    gw2->CopyRowsFromVec(w2_grad_vec_temp);
    gw2->AddMat(mmt,*gw2);
    // delete pointers
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_input.begin(); i!=reshaped_input.end(); ++i)
      delete *i;
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_diff.begin(); i!=reshaped_diff.end(); ++i)
      delete *i;
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_temp_w1.begin(); i!=reshaped_temp_w1.end(); ++i)
      delete *i;
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_temp_w2.begin(); i!=reshaped_temp_w2.end(); ++i)
      delete *i;
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_w1_grad_batch.begin(); i!=reshaped_w1_grad_batch.end(); ++i)
      delete *i;
    for(std::vector< CuSubMatrix<BaseFloat>* >::iterator i=reshaped_w2_grad_batch.begin(); i!=reshaped_w2_grad_batch.end(); ++i)
      delete *i;
    delete w1; delete w2;
  }


  
  void PropagateFnc(const CuMatrixBase<BaseFloat> &in, CuMatrixBase<BaseFloat> *out) {
    int DEBUG = 0;

    static bool do_stream_reset = false;
    if (nstream_ == 0) {
      do_stream_reset = true;
      nstream_ = 1; // Karel: we are in nnet-forward, so 1 stream,
      prev_nnet_state_.Resize(nstream_, 7*ncell_ + 1*nrecur_, kSetZero);
      KALDI_LOG << "Running nnet-forward with per-utterance LSTM-state reset";
    }
    if (do_stream_reset) prev_nnet_state_.SetZero();
    KALDI_ASSERT(nstream_ > 0);

    KALDI_ASSERT(in.NumRows() % nstream_ == 0);
    int32 T = in.NumRows() / nstream_;
    int32 S = nstream_;
    
    //KALDI_LOG<<"0:forward pass history, [1, T]:current sequence, T+1:dummy";
    //KALDI_LOG<<"T:"<<T<<"\t S:"<<S;
    // 0:forward pass history, [1, T]:current sequence, T+1:dummy
    propagate_buf_.Resize((T+2)*S, 7 * ncell_ + nrecur_, kSetZero);
    propagate_buf_.RowRange(0*S,S).CopyFromMat(prev_nnet_state_);
    

    //KALDI_LOG<<"disassemble entire neuron activation buffer into different neurons";
    //KALDI_LOG<<"cell dim"<<ncell_<<"\t recurrent dim"<<nrecur_;
    //KALDI_LOG<<"i1:"<<i1_<<"\t i2:"<<i2_;
    //KALDI_LOG<<"cj1:"<<cj1_<<"\t cj2:"<<cj2_;
    //KALDI_LOG<<"rj1:"<<rj1_<<"\t rj2:"<<rj2_;
    //KALDI_LOG<<"propagate_buf_ size:"<<"("<<propagate_buf_.NumRows()<<","<<propagate_buf_.NumCols()<<")";
    // disassemble entire neuron activation buffer into different neurons
    CuSubMatrix<BaseFloat> YG(propagate_buf_.ColRange(0*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YI(propagate_buf_.ColRange(1*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YF(propagate_buf_.ColRange(2*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YO(propagate_buf_.ColRange(3*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YC(propagate_buf_.ColRange(4*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YH(propagate_buf_.ColRange(5*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YM(propagate_buf_.ColRange(6*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YR(propagate_buf_.ColRange(7*ncell_, nrecur_));

    CuSubMatrix<BaseFloat> YGIFO(propagate_buf_.ColRange(0, 4*ncell_));

    int32 batch_size=in.NumRows();
    //KALDI_LOG<<"batch size:"<<batch_size;
    /// x -> g, i, f, o, not recurrent, do it all in once
    //KALDI_LOG<<"x -> g, i, f, o, not recurrent, do it all in once";
    //YGIFO.RowRange(1*S,T*S).AddMatMat(1.0, in, kNoTrans, w_gifo_x_, kTrans, 0.0);
    //KALDI_LOG<<"input num rows:"<<in.NumRows()<<"\t input num cols:"<<in.NumCols();
    //KALDI_LOG<<"i1:"<<i1_<<"\t i2:"<<i2_;
    
	const CuSubMatrix<BaseFloat>& YG_tmp = YG.RowRange(1*S,T*S);
    const CuSubMatrix<BaseFloat>& YI_tmp = YI.RowRange(1*S,T*S);
    const CuSubMatrix<BaseFloat>& YF_tmp = YF.RowRange(1*S,T*S);
    const CuSubMatrix<BaseFloat>& YO_tmp = YO.RowRange(1*S,T*S);
    CuSubMatrix<BaseFloat>& YG_tt = const_cast<CuSubMatrix<BaseFloat>&>(YG_tmp);
    CuSubMatrix<BaseFloat>& YI_tt = const_cast<CuSubMatrix<BaseFloat>&>(YI_tmp);
    CuSubMatrix<BaseFloat>& YF_tt = const_cast<CuSubMatrix<BaseFloat>&>(YF_tmp);
    CuSubMatrix<BaseFloat>& YO_tt = const_cast<CuSubMatrix<BaseFloat>&>(YO_tmp);
    // y -> g
    //KALDI_LOG<<"y -> g";
    TuckerFeedForward(w_g_x_mode_1_, w_g_x_mode_2_,
                      in, &YG_tt);
    // y -> i
    //KALDI_LOG<<"y -> i";
    TuckerFeedForward(w_i_x_mode_1_, w_i_x_mode_2_,
                      in, &YI_tt);
    // y -> f
    //KALDI_LOG<<"y -> f";
    TuckerFeedForward(w_f_x_mode_1_, w_f_x_mode_2_,
                      in, &YF_tt);
    // y -> o
    //KALDI_LOG<<"y -> o";
    TuckerFeedForward(w_o_x_mode_1_, w_o_x_mode_2_,
                      in, &YO_tt);

    //// LSTM forward dropout
    //// Google paper 2014: Recurrent Neural Network Regularization
    //// by Wojciech Zaremba, Ilya Sutskever, Oriol Vinyals
    //if (dropout_rate_ != 0.0) {
    //  dropout_mask_.Resize(in.NumRows(), 4*ncell_, kUndefined);
    //  dropout_mask_.SetRandUniform();   // [0,1]
    //  dropout_mask_.Add(-dropout_rate_);  // [-dropout_rate, 1-dropout_rate_],
    //  dropout_mask_.ApplyHeaviside();   // -tive -> 0.0, +tive -> 1.0
    //  YGIFO.RowRange(1*S,T*S).MulElements(dropout_mask_);
    //}

    /// bias -> g, i, f, o
    YGIFO.RowRange(1*S,T*S).AddVecToRows(1.0, bias_);


    for (int t = 1; t <= T; t++) {
      // multistream buffers for current time-step
      CuSubMatrix<BaseFloat> y_g(YG.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_i(YI.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_f(YF.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_o(YO.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_c(YC.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_h(YH.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_m(YM.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_r(YR.RowRange(t*S,S));

      //CuSubMatrix<BaseFloat> y_gifo(YGIFO.RowRange(t*S,S));

      /// r(t-1) -> g, i, f, o
      //KALDI_LOG<<"r(t-1) -> g, i, f, o";
      //y_gifo.AddMatMat(1.0, YR.RowRange((t-1)*S,S), kNoTrans, w_gifo_r_, kTrans, 1.0);
      // r(t-1) -> g
      TuckerFeedForward(w_g_r_mode_1_, w_g_r_mode_2_,
                        YR.RowRange((t-1)*S,S), &y_g);
      // r(t-1) -> i
      TuckerFeedForward(w_i_r_mode_1_, w_i_r_mode_2_,
                        YR.RowRange((t-1)*S,S), &y_i);
      // r(t-1) -> f
      TuckerFeedForward(w_f_r_mode_1_, w_f_r_mode_2_,
                        YR.RowRange((t-1)*S,S), &y_f);
      // r(t-1) -> o
      TuckerFeedForward(w_o_r_mode_1_, w_o_r_mode_2_,
                        YR.RowRange((t-1)*S,S), &y_o);

      /// c(t-1) -> i(t) via peephole
      y_i.AddMatDiagVec(1.0, YC.RowRange((t-1)*S,S), kNoTrans, peephole_i_c_, 1.0);

      /// c(t-1) -> f(t) via peephole
      y_f.AddMatDiagVec(1.0, YC.RowRange((t-1)*S,S), kNoTrans, peephole_f_c_, 1.0);

      /// i, f sigmoid squashing
      y_i.Sigmoid(y_i);
      y_f.Sigmoid(y_f);

      /// g tanh squashing
      y_g.Tanh(y_g);

      /// g -> c
      y_c.AddMatMatElements(1.0, y_g, y_i, 0.0);

      /// c(t-1) -> c(t) via forget-gate
      y_c.AddMatMatElements(1.0, YC.RowRange((t-1)*S,S), y_f, 1.0);

      y_c.ApplyFloor(-50);   // optional clipping of cell activation
      y_c.ApplyCeiling(50);  // google paper Interspeech2014: LSTM for LVCSR

      /// h tanh squashing
      y_h.Tanh(y_c);

      /// c(t) -> o(t) via peephole (non-recurrent) & o squashing
      y_o.AddMatDiagVec(1.0, y_c, kNoTrans, peephole_o_c_, 1.0);

      /// o sigmoid squashing
      y_o.Sigmoid(y_o);

      /// h -> m via output gate
      y_m.AddMatMatElements(1.0, y_h, y_o, 0.0);

      /// m -> r
      //KALDI_LOG<<"m -> r";
      // y_r.AddMatMat(1.0, y_m, kNoTrans, w_r_m_, kTrans, 0.0);
      TuckerFeedForward(w_r_m_mode_1_, w_r_m_mode_2_,
                        y_m, &y_r);

      if (DEBUG) {
        std::cerr << "forward-pass frame " << t << "\n";
        std::cerr << "activation of g: " << y_g;
        std::cerr << "activation of i: " << y_i;
        std::cerr << "activation of f: " << y_f;
        std::cerr << "activation of o: " << y_o;
        std::cerr << "activation of c: " << y_c;
        std::cerr << "activation of h: " << y_h;
        std::cerr << "activation of m: " << y_m;
        std::cerr << "activation of r: " << y_r;
      }
    }

    // recurrent projection layer is also feed-forward as LSTM output
    out->CopyFromMat(YR.RowRange(1*S,T*S));

    // now the last frame state becomes previous network state for next batch
    prev_nnet_state_.CopyFromMat(propagate_buf_.RowRange(T*S,S));
  }

  void BackpropagateFnc(const CuMatrixBase<BaseFloat> &in, const CuMatrixBase<BaseFloat> &out,
              const CuMatrixBase<BaseFloat> &out_diff, CuMatrixBase<BaseFloat> *in_diff) {

    int DEBUG = 0;

    int32 T = in.NumRows() / nstream_;
    int32 S = nstream_;

    // disassemble propagated buffer into neurons
    CuSubMatrix<BaseFloat> YG(propagate_buf_.ColRange(0*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YI(propagate_buf_.ColRange(1*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YF(propagate_buf_.ColRange(2*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YO(propagate_buf_.ColRange(3*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YC(propagate_buf_.ColRange(4*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YH(propagate_buf_.ColRange(5*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YM(propagate_buf_.ColRange(6*ncell_, ncell_));
    CuSubMatrix<BaseFloat> YR(propagate_buf_.ColRange(7*ncell_, nrecur_));

    // 0:dummy, [1,T] frames, T+1 backward pass history
    backpropagate_buf_.Resize((T+2)*S, 7 * ncell_ + nrecur_, kSetZero);

    // disassemble backpropagate buffer into neurons
    CuSubMatrix<BaseFloat> DG(backpropagate_buf_.ColRange(0*ncell_, ncell_));
    CuSubMatrix<BaseFloat> DI(backpropagate_buf_.ColRange(1*ncell_, ncell_));
    CuSubMatrix<BaseFloat> DF(backpropagate_buf_.ColRange(2*ncell_, ncell_));
    CuSubMatrix<BaseFloat> DO(backpropagate_buf_.ColRange(3*ncell_, ncell_));
    CuSubMatrix<BaseFloat> DC(backpropagate_buf_.ColRange(4*ncell_, ncell_));
    CuSubMatrix<BaseFloat> DH(backpropagate_buf_.ColRange(5*ncell_, ncell_));
    CuSubMatrix<BaseFloat> DM(backpropagate_buf_.ColRange(6*ncell_, ncell_));
    CuSubMatrix<BaseFloat> DR(backpropagate_buf_.ColRange(7*ncell_, nrecur_));

    CuSubMatrix<BaseFloat> DGIFO(backpropagate_buf_.ColRange(0, 4*ncell_));

    // projection layer to LSTM output is not recurrent, so backprop it all in once
    DR.RowRange(1*S,T*S).CopyFromMat(out_diff);

    for (int t = T; t >= 1; t--) {
      CuSubMatrix<BaseFloat> y_g(YG.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_i(YI.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_f(YF.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_o(YO.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_c(YC.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_h(YH.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_m(YM.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> y_r(YR.RowRange(t*S,S));

      CuSubMatrix<BaseFloat> d_g(DG.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> d_i(DI.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> d_f(DF.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> d_o(DO.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> d_c(DC.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> d_h(DH.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> d_m(DM.RowRange(t*S,S));
      CuSubMatrix<BaseFloat> d_r(DR.RowRange(t*S,S));

      /// r
      //   Version 1 (precise gradients):
      //   backprop error from g(t+1), i(t+1), f(t+1), o(t+1) to r(t)
      // d_r.AddMatMat(1.0, DGIFO.RowRange((t+1)*S,S), kNoTrans, w_gifo_r_, kNoTrans, 1.0);
      // d_r : rj1_ * rj2_
      CuMatrix<BaseFloat> d_r_tmp(S,ncell_);
      d_r_tmp.SetZero();
      // g(t+1) -> r(t) : d_r_g
      TuckerBackProp(w_g_r_mode_1_, w_g_r_mode_2_,
                     DG.RowRange((t+1)*S,S), &d_r_tmp);
      d_r.AddMat(1.0,d_r_tmp);
      d_r_tmp.SetZero();
      // i(t+1) -> r(t) : d_r_i
      TuckerBackProp(w_i_r_mode_1_, w_i_r_mode_2_,
                     DI.RowRange((t+1)*S,S), &d_r_tmp);
      d_r.AddMat(1.0,d_r_tmp);
      d_r_tmp.SetZero();
      // f(t+1) -> r(t) : d_r_f
      TuckerBackProp(w_f_r_mode_1_, w_f_r_mode_2_,
                     DF.RowRange((t+1)*S,S), &d_r_tmp);
      d_r.AddMat(1.0,d_r_tmp);
      d_r_tmp.SetZero();
      // o(t+1) -> o(t) : d_r_o
      TuckerBackProp(w_o_r_mode_1_, w_o_r_mode_2_,
                     DO.RowRange((t+1)*S,S), &d_r_tmp);
      d_r.AddMat(1.0,d_r_tmp);
      // d_r_tmp.SetZero();

      /*
      //   Version 2 (Alex Graves' PhD dissertation):
      //   only backprop g(t+1) to r(t)
      CuSubMatrix<BaseFloat> w_g_r_(w_gifo_r_.RowRange(0, ncell_));
      d_r.AddMatMat(1.0, DG.RowRange((t+1)*S,S), kNoTrans, w_g_r_, kNoTrans, 1.0);
      */

      /*
      //   Version 3 (Felix Gers' PhD dissertation):
      //   truncate gradients of g(t+1), i(t+1), f(t+1), o(t+1) once they leak out memory block
      //   CEC(with forget connection) is the only "error-bridge" through time
      */

      /// r -> m
      // d_m.AddMatMat(1.0, d_r, kNoTrans, w_r_m_, kNoTrans, 0.0);
      // d_r : rj1_ * rj2_
      // d_m : cj1_ * cj2_
      TuckerBackProp(w_r_m_mode_1_, w_r_m_mode_2_,
                     d_r, &d_m);

      // m -> h via output gate
      d_h.AddMatMatElements(1.0, d_m, y_o, 0.0);
      d_h.DiffTanh(y_h, d_h);

      // o
      d_o.AddMatMatElements(1.0, d_m, y_h, 0.0);
      d_o.DiffSigmoid(y_o, d_o);

      // c
      // 1. diff from h(t)
      // 2. diff from c(t+1) (via forget-gate between CEC)
      // 3. diff from i(t+1) (via peephole)
      // 4. diff from f(t+1) (via peephole)
      // 5. diff from o(t)   (via peephole, not recurrent)
      d_c.AddMat(1.0, d_h);
      d_c.AddMatMatElements(1.0, DC.RowRange((t+1)*S,S), YF.RowRange((t+1)*S,S), 1.0);
      d_c.AddMatDiagVec(1.0, DI.RowRange((t+1)*S,S), kNoTrans, peephole_i_c_, 1.0);
      d_c.AddMatDiagVec(1.0, DF.RowRange((t+1)*S,S), kNoTrans, peephole_f_c_, 1.0);
      d_c.AddMatDiagVec(1.0, d_o                   , kNoTrans, peephole_o_c_, 1.0);

      // f
      d_f.AddMatMatElements(1.0, d_c, YC.RowRange((t-1)*S,S), 0.0);
      d_f.DiffSigmoid(y_f, d_f);

      // i
      d_i.AddMatMatElements(1.0, d_c, y_g, 0.0);
      d_i.DiffSigmoid(y_i, d_i);

      // c -> g via input gate
      d_g.AddMatMatElements(1.0, d_c, y_i, 0.0);
      d_g.DiffTanh(y_g, d_g);

      // debug info
      if (DEBUG) {
        std::cerr << "backward-pass frame " << t << "\n";
        std::cerr << "derivative wrt input r " << d_r;
        std::cerr << "derivative wrt input m " << d_m;
        std::cerr << "derivative wrt input h " << d_h;
        std::cerr << "derivative wrt input o " << d_o;
        std::cerr << "derivative wrt input c " << d_c;
        std::cerr << "derivative wrt input f " << d_f;
        std::cerr << "derivative wrt input i " << d_i;
        std::cerr << "derivative wrt input g " << d_g;
      }
    }

    /// g,i,f,o -> x, do it all in once
    // in_diff->AddMatMat(1.0, DGIFO.RowRange(1*S,T*S), kNoTrans, w_gifo_x_, kNoTrans, 0.0);
    int32 batch_size = out_diff.NumRows();

    CuMatrix<BaseFloat> indiff_tmp(batch_size,i1_ * i2_);
    indiff_tmp.SetZero();
    // g -> x 
    TuckerBackProp(w_g_x_mode_1_, w_g_x_mode_2_,
                   DG.RowRange(1*S,T*S), &indiff_tmp);
    in_diff->AddMat(1.0,indiff_tmp);
    indiff_tmp.SetZero();
    // i -> x
    TuckerBackProp(w_i_x_mode_1_, w_i_x_mode_2_,
                   DI.RowRange(1*S,T*S), &indiff_tmp);
    in_diff->AddMat(1.0,indiff_tmp);
    indiff_tmp.SetZero();
    // f -> x
    TuckerBackProp(w_f_x_mode_1_, w_f_x_mode_2_,
                   DF.RowRange(1*S,T*S), &indiff_tmp);
    in_diff->AddMat(1.0,indiff_tmp);
    indiff_tmp.SetZero();
    // o -> x
    TuckerBackProp(w_o_x_mode_1_, w_o_x_mode_2_,
                   DO.RowRange(1*S,T*S), &indiff_tmp);
    in_diff->AddMat(1.0,indiff_tmp);
    // indiff_tmp.SetZero();

    //// backward pass dropout
    //if (dropout_rate_ != 0.0) {
    //  in_diff->MulElements(dropout_mask_);
    //}

    /// calculate delta
    /// gradients
    const BaseFloat mmt = opts_.momentum;

    /// weight x -> g, i, f, o 
    // w_gifo_x_corr_.AddMatMat(1.0, DGIFO.RowRange(1*S,T*S), kTrans, in, kNoTrans, mmt);
    // x -> g
    // DG_x : (b,cj1*cj2*1)
    // w_g_x_corr_
    TuckerCalcGrad(w_g_x_mode_1_, w_g_x_mode_2_,
                   in, DG.RowRange(1*S,T*S),
                   &w_g_x_corr_mode_1_, &w_g_x_mode_2_,
                   mmt);
    // x -> i
    // DI_x : (b,cj1*cj2*1)
    // w_i_x_corr_ 
    TuckerCalcGrad(w_i_x_mode_1_, w_i_x_mode_2_,
                   in, DI.RowRange(1*S,T*S),
                   &w_i_x_corr_mode_1_, &w_i_x_mode_2_,
                   mmt);
    // x -> f
    // DF_x : (b,cj1*cj2*1)
    // w_f_x_corr_
    TuckerCalcGrad(w_f_x_mode_1_, w_f_x_mode_2_,
                   in, DF.RowRange(1*S,T*S),
                   &w_f_x_corr_mode_1_, &w_f_x_mode_2_,
                   mmt);
    // x -> o
    // DO_x : (b,cj1*cj2*1)
    // w_o_x_corr_
    TuckerCalcGrad(w_o_x_mode_1_, w_o_x_mode_2_,
                   in, DO.RowRange(1*S,T*S),
                   &w_o_x_corr_mode_1_, &w_o_x_mode_2_,
                   mmt);

    /// recurrent weight r -> g, i, f, o
    // w_gifo_r_corr_.AddMatMat(1.0, DGIFO.RowRange(1*S,T*S), kTrans, YR.RowRange(0*S,T*S), kNoTrans, mmt);
    // r -> g
    // DG : (b,cj1*cj2*1)
    // w_g_r_corr_
    TuckerCalcGrad(w_g_r_mode_1_, w_g_r_mode_2_,
                   YR.RowRange(0*S,T*S), DG.RowRange(1*S,T*S),
                   &w_g_r_corr_mode_1_ ,&w_g_r_corr_mode_2_ ,
                   mmt); 
    // r -> i
    // DI : (b,cj1*cj2*1)
    // w_i_r_corr_
    TuckerCalcGrad(w_i_r_mode_1_, w_i_r_mode_2_,
                   YR.RowRange(0*S,T*S), DI.RowRange(1*S,T*S),
                   &w_i_r_corr_mode_1_ ,&w_i_r_corr_mode_2_ ,
                   mmt); 
    // r -> f
    // DF : (b,cj1*cj2*1)
    // w_f_r_corr_
    TuckerCalcGrad(w_f_r_mode_1_, w_f_r_mode_2_,
                   YR.RowRange(0*S,T*S), DF.RowRange(1*S,T*S),
                   &w_f_r_corr_mode_1_ ,&w_f_r_corr_mode_2_ ,
                   mmt); 
    // r -> o
    // DO : (b,cj1*cj2*1)
    // w_o_r_corr_
    TuckerCalcGrad(w_o_r_mode_1_, w_o_r_mode_2_,
                   YR.RowRange(0*S,T*S), DO.RowRange(1*S,T*S),
                   &w_o_r_corr_mode_1_ ,&w_o_r_corr_mode_2_ ,
                   mmt); 

   
    // bias of g, i, f, o
    bias_corr_.AddRowSumMat(1.0, DGIFO.RowRange(1*S,T*S), mmt);

    // recurrent peephole c -> i
    peephole_i_c_corr_.AddDiagMatMat(1.0, DI.RowRange(1*S,T*S), kTrans,
                                          YC.RowRange(0*S,T*S), kNoTrans, mmt);
    // recurrent peephole c -> f
    peephole_f_c_corr_.AddDiagMatMat(1.0, DF.RowRange(1*S,T*S), kTrans,
                                          YC.RowRange(0*S,T*S), kNoTrans, mmt);
    // peephole c -> o
    peephole_o_c_corr_.AddDiagMatMat(1.0, DO.RowRange(1*S,T*S), kTrans,
                                          YC.RowRange(1*S,T*S), kNoTrans, mmt);

    // w_r_m_corr_.AddMatMat(1.0, DR.RowRange(1*S,T*S), kTrans, YM.RowRange(1*S,T*S), kNoTrans, mmt);
    TuckerCalcGrad(w_r_m_mode_1_, w_r_m_mode_2_,
                   YM.RowRange(1*S,T*S), DR.RowRange(1*S,T*S),
                   &w_r_m_corr_mode_1_ , &w_r_m_corr_mode_2_,
                   mmt);

    if (clip_gradient_ > 0.0) {
      /*
      w_gifo_x_corr_.ApplyFloor(-clip_gradient_);
      w_gifo_x_corr_.ApplyCeiling(clip_gradient_);
      w_gifo_r_corr_.ApplyFloor(-clip_gradient_);
      w_gifo_r_corr_.ApplyCeiling(clip_gradient_);
      */
      w_g_x_corr_mode_1_.ApplyFloor(-clip_gradient_);
      w_g_x_corr_mode_1_.ApplyCeiling(clip_gradient_);
      w_g_x_corr_mode_2_.ApplyFloor(-clip_gradient_);
      w_g_x_corr_mode_2_.ApplyCeiling(clip_gradient_);
      w_i_x_corr_mode_1_.ApplyFloor(-clip_gradient_);
      w_i_x_corr_mode_1_.ApplyCeiling(clip_gradient_);
      w_i_x_corr_mode_2_.ApplyFloor(-clip_gradient_);
      w_i_x_corr_mode_2_.ApplyCeiling(clip_gradient_);
      w_f_x_corr_mode_1_.ApplyFloor(-clip_gradient_);
      w_f_x_corr_mode_1_.ApplyCeiling(clip_gradient_);
      w_f_x_corr_mode_2_.ApplyFloor(-clip_gradient_);
      w_f_x_corr_mode_2_.ApplyCeiling(clip_gradient_);
      w_o_x_corr_mode_1_.ApplyFloor(-clip_gradient_);
      w_o_x_corr_mode_1_.ApplyCeiling(clip_gradient_);
      w_o_x_corr_mode_2_.ApplyFloor(-clip_gradient_);
      w_o_x_corr_mode_2_.ApplyCeiling(clip_gradient_);

      w_g_r_corr_mode_1_.ApplyFloor(-clip_gradient_);
      w_g_r_corr_mode_1_.ApplyCeiling(clip_gradient_);
      w_g_r_corr_mode_2_.ApplyFloor(-clip_gradient_);
      w_g_r_corr_mode_2_.ApplyCeiling(clip_gradient_);
      w_i_r_corr_mode_1_.ApplyFloor(-clip_gradient_);
      w_i_r_corr_mode_1_.ApplyCeiling(clip_gradient_);
      w_i_r_corr_mode_2_.ApplyFloor(-clip_gradient_);
      w_i_r_corr_mode_2_.ApplyCeiling(clip_gradient_);
      w_f_r_corr_mode_1_.ApplyFloor(-clip_gradient_);
      w_f_r_corr_mode_1_.ApplyCeiling(clip_gradient_);
      w_f_r_corr_mode_2_.ApplyFloor(-clip_gradient_);
      w_f_r_corr_mode_2_.ApplyCeiling(clip_gradient_);
      w_o_r_corr_mode_1_.ApplyFloor(-clip_gradient_);
      w_o_r_corr_mode_1_.ApplyCeiling(clip_gradient_);
      w_o_r_corr_mode_2_.ApplyFloor(-clip_gradient_);
      w_o_r_corr_mode_2_.ApplyCeiling(clip_gradient_);

      bias_corr_.ApplyFloor(-clip_gradient_);
      bias_corr_.ApplyCeiling(clip_gradient_);
      /*
      w_r_m_corr_.ApplyFloor(-clip_gradient_);
      w_r_m_corr_.ApplyCeiling(clip_gradient_);
      */
      w_r_m_corr_mode_1_.ApplyFloor(-clip_gradient_);
      w_r_m_corr_mode_1_.ApplyCeiling(clip_gradient_);
      w_r_m_corr_mode_2_.ApplyFloor(-clip_gradient_);
      w_r_m_corr_mode_2_.ApplyCeiling(clip_gradient_);

      peephole_i_c_corr_.ApplyFloor(-clip_gradient_);
      peephole_i_c_corr_.ApplyCeiling(clip_gradient_);
      peephole_f_c_corr_.ApplyFloor(-clip_gradient_);
      peephole_f_c_corr_.ApplyCeiling(clip_gradient_);
      peephole_o_c_corr_.ApplyFloor(-clip_gradient_);
      peephole_o_c_corr_.ApplyCeiling(clip_gradient_);
    }

    if (DEBUG) {
      std::cerr << "gradients(with optional momentum): \n";
      std::cerr << "w_g_x_corr_mode_1_ " << w_g_x_corr_mode_1_;
      std::cerr << "w_g_x_corr_mode_2_ " << w_g_x_corr_mode_2_;
      std::cerr << "w_i_x_corr_mode_1_ " << w_i_x_corr_mode_1_;
      std::cerr << "w_i_x_corr_mode_2_ " << w_i_x_corr_mode_2_;
      std::cerr << "w_f_x_corr_mode_1_ " << w_f_x_corr_mode_1_;
      std::cerr << "w_f_x_corr_mode_2_ " << w_f_x_corr_mode_2_;
      std::cerr << "w_o_x_corr_mode_1_ " << w_o_x_corr_mode_1_;
      std::cerr << "w_o_x_corr_mode_2_ " << w_o_x_corr_mode_2_;

      std::cerr << "w_g_r_corr_mode_1_ " << w_g_r_corr_mode_1_;
      std::cerr << "w_g_r_corr_mode_2_ " << w_g_r_corr_mode_2_;
      std::cerr << "w_i_r_corr_mode_1_ " << w_i_r_corr_mode_1_;
      std::cerr << "w_i_r_corr_mode_2_ " << w_i_r_corr_mode_2_;
      std::cerr << "w_f_r_corr_mode_1_ " << w_f_r_corr_mode_1_;
      std::cerr << "w_f_r_corr_mode_2_ " << w_f_r_corr_mode_2_;
      std::cerr << "w_o_r_corr_mode_1_ " << w_o_r_corr_mode_1_;
      std::cerr << "w_o_r_corr_mode_2_ " << w_o_r_corr_mode_2_;

      std::cerr << "bias_corr_ " << bias_corr_;

      //std::cerr << "w_r_m_corr_ " << w_r_m_corr_;
      std::cerr << "w_r_m_corr_mode_1_ " << w_r_m_corr_mode_1_;
      std::cerr << "w_r_m_corr_mode_2_ " << w_r_m_corr_mode_2_;

      std::cerr << "peephole_i_c_corr_ " << peephole_i_c_corr_;
      std::cerr << "peephole_f_c_corr_ " << peephole_f_c_corr_;
      std::cerr << "peephole_o_c_corr_ " << peephole_o_c_corr_;
    }
  }

  void Update(const CuMatrixBase<BaseFloat> &input, const CuMatrixBase<BaseFloat> &diff) {
    const BaseFloat lr  = opts_.learn_rate;

    // w_gifo_x_.AddMat(-lr, w_gifo_x_corr_);
    // w_gifo_r_.AddMat(-lr, w_gifo_r_corr_);
    w_g_x_mode_1_.AddMat(-lr, w_g_x_corr_mode_1_);
    w_g_x_mode_2_.AddMat(-lr, w_g_x_corr_mode_2_);
    w_i_x_mode_1_.AddMat(-lr, w_i_x_corr_mode_1_);
    w_i_x_mode_2_.AddMat(-lr, w_i_x_corr_mode_2_);
    w_f_x_mode_1_.AddMat(-lr, w_f_x_corr_mode_1_);
    w_f_x_mode_2_.AddMat(-lr, w_f_x_corr_mode_2_);
    w_o_x_mode_1_.AddMat(-lr, w_o_x_corr_mode_1_);
    w_o_x_mode_2_.AddMat(-lr, w_o_x_corr_mode_2_);

    w_g_r_mode_1_.AddMat(-lr, w_g_r_corr_mode_1_);
    w_g_r_mode_2_.AddMat(-lr, w_g_r_corr_mode_2_);
    w_i_r_mode_1_.AddMat(-lr, w_i_r_corr_mode_1_);
    w_i_r_mode_2_.AddMat(-lr, w_i_r_corr_mode_2_);
    w_f_r_mode_1_.AddMat(-lr, w_f_r_corr_mode_1_);
    w_f_r_mode_2_.AddMat(-lr, w_f_r_corr_mode_2_);
    w_o_r_mode_1_.AddMat(-lr, w_o_r_corr_mode_1_);
    w_o_r_mode_2_.AddMat(-lr, w_o_r_corr_mode_2_);

    bias_.AddVec(-lr, bias_corr_, 1.0);

    peephole_i_c_.AddVec(-lr, peephole_i_c_corr_, 1.0);
    peephole_f_c_.AddVec(-lr, peephole_f_c_corr_, 1.0);
    peephole_o_c_.AddVec(-lr, peephole_o_c_corr_, 1.0);

    // w_r_m_.AddMat(-lr, w_r_m_corr_);
    w_r_m_mode_1_.AddMat(-lr, w_r_m_corr_mode_1_);
    w_r_m_mode_2_.AddMat(-lr, w_r_m_corr_mode_2_);

//    /*
//      Here we deal with the famous "vanishing & exploding difficulties" in RNN learning.
//
//      *For gradients vanishing*
//      LSTM architecture introduces linear CEC as the "error bridge" across long time distance
//      solving vanishing problem.
//
//      *For gradients exploding*
//      LSTM is still vulnerable to gradients explosing in BPTT(with large weight & deep time expension).
//      To prevent this, we tried L2 regularization, which didn't work well
//
//      Our approach is a *modified* version of Max Norm Regularization:
//      For each nonlinear neuron,
//      1. fan-in weights & bias model a seperation hyper-plane: W x + b = 0
//      2. squashing function models a differentiable nonlinear slope around this hyper-plane.
//
//      Conventional max norm regularization scale W to keep its L2 norm bounded,
//      As a modification, we scale down large (W & b) *simultaneously*, this:
//      1. keeps all fan-in weights small, prevents gradients from exploding during backward-pass.
//      2. keeps the location of the hyper-plane unchanged, so we don't wipe out already learned knowledge.
//      3. shrinks the "normal" of the hyper-plane, smooths the nonlinear slope, improves generalization.
//      4. makes the network *well-conditioned* (weights are constrained in a reasonible range).
//
//      We've observed faster convergence and performance gain by doing this.
//    */
//
//    int DEBUG = 0;
//    BaseFloat max_norm = 1.0;   // weights with large L2 norm may cause exploding in deep BPTT expensions
//                  // TODO: move this config to opts_
//    CuMatrix<BaseFloat> L2_gifo_x(w_gifo_x_);
//    CuMatrix<BaseFloat> L2_gifo_r(w_gifo_r_);
//    L2_gifo_x.MulElements(w_gifo_x_);
//    L2_gifo_r.MulElements(w_gifo_r_);
//
//    CuVector<BaseFloat> L2_norm_gifo(L2_gifo_x.NumRows());
//    L2_norm_gifo.AddColSumMat(1.0, L2_gifo_x, 0.0);
//    L2_norm_gifo.AddColSumMat(1.0, L2_gifo_r, 1.0);
//    L2_norm_gifo.Range(1*ncell_, ncell_).AddVecVec(1.0, peephole_i_c_, peephole_i_c_, 1.0);
//    L2_norm_gifo.Range(2*ncell_, ncell_).AddVecVec(1.0, peephole_f_c_, peephole_f_c_, 1.0);
//    L2_norm_gifo.Range(3*ncell_, ncell_).AddVecVec(1.0, peephole_o_c_, peephole_o_c_, 1.0);
//    L2_norm_gifo.ApplyPow(0.5);
//
//    CuVector<BaseFloat> shrink(L2_norm_gifo);
//    shrink.Scale(1.0/max_norm);
//    shrink.ApplyFloor(1.0);
//    shrink.InvertElements();
//
//    w_gifo_x_.MulRowsVec(shrink);
//    w_gifo_r_.MulRowsVec(shrink);
//    bias_.MulElements(shrink);
//
//    peephole_i_c_.MulElements(shrink.Range(1*ncell_, ncell_));
//    peephole_f_c_.MulElements(shrink.Range(2*ncell_, ncell_));
//    peephole_o_c_.MulElements(shrink.Range(3*ncell_, ncell_));
//
//    if (DEBUG) {
//      if (shrink.Min() < 0.95) {   // we dont want too many trivial logs here
//        std::cerr << "gifo shrinking coefs: " << shrink;
//      }
//    }
//
  }

 private:
  // dims
  int32 ncell_;
  int32 nrecur_;  ///< recurrent projection layer dim
  int32 nstream_;

  int32 input_dim_,output_dim_;

  int32 i1_,i2_;     // input dim 
  int32 cj1_,cj2_;   // cell dim
  int32 rj1_,rj2_;   // recurrnet projection layer dim

  CuMatrix<BaseFloat> prev_nnet_state_;

  // gradient-clipping value,
  BaseFloat clip_gradient_;

  // non-recurrent dropout
  //BaseFloat dropout_rate_;
  //CuMatrix<BaseFloat> dropout_mask_;

  // feed-forward connections: from x to [g, i, f, o]
  CuMatrix<BaseFloat> w_g_x_mode_1_;
  CuMatrix<BaseFloat> w_g_x_mode_2_;
  CuMatrix<BaseFloat> w_g_x_corr_mode_1_;
  CuMatrix<BaseFloat> w_g_x_corr_mode_2_;
  CuMatrix<BaseFloat> w_i_x_mode_1_;
  CuMatrix<BaseFloat> w_i_x_mode_2_;
  CuMatrix<BaseFloat> w_i_x_corr_mode_1_;
  CuMatrix<BaseFloat> w_i_x_corr_mode_2_;
  CuMatrix<BaseFloat> w_f_x_mode_1_;
  CuMatrix<BaseFloat> w_f_x_mode_2_;
  CuMatrix<BaseFloat> w_f_x_corr_mode_1_;
  CuMatrix<BaseFloat> w_f_x_corr_mode_2_;
  CuMatrix<BaseFloat> w_o_x_mode_1_;
  CuMatrix<BaseFloat> w_o_x_mode_2_;
  CuMatrix<BaseFloat> w_o_x_corr_mode_1_;
  CuMatrix<BaseFloat> w_o_x_corr_mode_2_;

  // recurrent projection connections: from r to [g, i, f, o]
  CuMatrix<BaseFloat> w_g_r_mode_1_;
  CuMatrix<BaseFloat> w_g_r_mode_2_;
  CuMatrix<BaseFloat> w_g_r_corr_mode_1_;
  CuMatrix<BaseFloat> w_g_r_corr_mode_2_;
  CuMatrix<BaseFloat> w_i_r_mode_1_;
  CuMatrix<BaseFloat> w_i_r_mode_2_;
  CuMatrix<BaseFloat> w_i_r_corr_mode_1_;
  CuMatrix<BaseFloat> w_i_r_corr_mode_2_;
  CuMatrix<BaseFloat> w_f_r_mode_1_;
  CuMatrix<BaseFloat> w_f_r_mode_2_;
  CuMatrix<BaseFloat> w_f_r_corr_mode_1_;
  CuMatrix<BaseFloat> w_f_r_corr_mode_2_;
  CuMatrix<BaseFloat> w_o_r_mode_1_;
  CuMatrix<BaseFloat> w_o_r_mode_2_;
  CuMatrix<BaseFloat> w_o_r_corr_mode_1_;
  CuMatrix<BaseFloat> w_o_r_corr_mode_2_;

  // biases of [g, i, f, o]
  CuVector<BaseFloat> bias_;
  CuVector<BaseFloat> bias_corr_;

  // peephole from c to i, f, g
  // peephole connections are block-internal, so we use vector form
  CuVector<BaseFloat> peephole_i_c_;
  CuVector<BaseFloat> peephole_f_c_;
  CuVector<BaseFloat> peephole_o_c_;

  CuVector<BaseFloat> peephole_i_c_corr_;
  CuVector<BaseFloat> peephole_f_c_corr_;
  CuVector<BaseFloat> peephole_o_c_corr_;

  // projection layer r: from m to r
  CuMatrix<BaseFloat> w_r_m_mode_1_;
  CuMatrix<BaseFloat> w_r_m_mode_2_;
  CuMatrix<BaseFloat> w_r_m_corr_mode_1_;
  CuMatrix<BaseFloat> w_r_m_corr_mode_2_;

  // propagate buffer: output of [g, i, f, o, c, h, m, r]
  CuMatrix<BaseFloat> propagate_buf_;

  // back-propagate buffer: diff-input of [g, i, f, o, c, h, m, r]
  CuMatrix<BaseFloat> backpropagate_buf_;

};
} // namespace nnet1
} // namespace kaldi

#endif
