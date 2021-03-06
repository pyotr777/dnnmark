// The MIT License (MIT)
//
// Copyright (c) 2016 Northeastern University
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef CORE_INCLUDE_LAYERS_CONV_LAYER_H_
#define CORE_INCLUDE_LAYERS_CONV_LAYER_H_

#include "dnn_layer.h"
#include <iostream>

namespace dnnmark {

template <typename T>
class ConvolutionLayer : public Layer<T> {
  // using declaration for calling member from base class
  using Layer<T>::p_dnnmark_;
  using Layer<T>::layer_id_;
  using Layer<T>::previous_layer_name_;
  using Layer<T>::input_dim_;
  using Layer<T>::output_dim_;
  using Layer<T>::bottom_desc_;
  using Layer<T>::top_desc_;
  using Layer<T>::data_manager_;

  using Layer<T>::num_bottoms_;
  using Layer<T>::bottoms_;
  using Layer<T>::bottom_chunk_ids_;
  using Layer<T>::bottom_diffs_;
  using Layer<T>::bottom_diff_chunk_ids_;

  using Layer<T>::num_tops_;
  using Layer<T>::tops_;
  using Layer<T>::top_chunk_ids_;
  using Layer<T>::top_diffs_;
  using Layer<T>::top_diff_chunk_ids_;

 private:
  ConvolutionParam conv_param_;

  // Convolution specific descriptor
  ConvolutionDesc<T> desc_;

  // Layer weights
  Data<T> *weights_;
  int weights_chunk_id_;
  Data<T> *weights_diff_;
  int weights_diff_chunk_id_;

  // Algorithm specific parameters
  ConvAlgo<T> conv_algo_;
  size_t fwd_workspace_size_;
  size_t bwd_data_workspace_size_;
  size_t bwd_filter_workspace_size_;
  Data<T> *fwd_workspace_;
  int fwd_workspace_id_;
  Data<T> *bwd_data_workspace_;
  int bwd_data_workspace_id_;
  Data<T> *bwd_filter_workspace_;
  int bwd_filter_workspace_id_;
  bool has_fwd_workspace_;
  bool has_bwd_data_workspace_;
  bool has_bwd_filter_workspace_;
 public:
  ConvolutionLayer(DNNMark<T> *p_dnnmark)
  : Layer<T>(p_dnnmark),
    conv_param_(), desc_(), conv_algo_() {
    Layer<T>::has_learnable_params_ = true;
    fwd_workspace_size_ = 0;
    bwd_data_workspace_size_ = 0;
    bwd_filter_workspace_size_ = 0;
    has_fwd_workspace_ = false;
    has_bwd_data_workspace_ = false;
    has_bwd_filter_workspace_ = false;
  }

  ~ConvolutionLayer() {
    // Free the workspace
    if (has_fwd_workspace_) {
      data_manager_->RemoveData(fwd_workspace_id_);
      has_fwd_workspace_ = false;
    }
    if (has_bwd_data_workspace_) {
      data_manager_->RemoveData(bwd_data_workspace_id_);
      has_bwd_data_workspace_ = false;
    }
    if (has_bwd_filter_workspace_) {
      data_manager_->RemoveData(bwd_filter_workspace_id_);
      has_bwd_filter_workspace_ = false;
    }
  }

  ConvolutionParam *getConvParam() { return &conv_param_; }

  void Setup() {
    // Set up indispensable stuff here
    Layer<T>::Setup();

    // Set convolution related descriptors
    desc_.Set(conv_param_, input_dim_.c_);

    // Set up convolution related data
    if (input_dim_.n_ != 0 && input_dim_.c_ != 0 &&
        input_dim_.h_ != 0 && input_dim_.w_ != 0) {
      //
      // Standalone mode
      //

      // Compute dimension of output data
      ComputeOutputDim();

      // Set top tensor
      top_desc_.Set(output_dim_.n_,
                    output_dim_.c_,
                    output_dim_.h_,
                    output_dim_.w_);

      // Prepare top data
      int top_size = output_dim_.n_ *
                     output_dim_.c_ *
                     output_dim_.h_ *
                     output_dim_.w_;
      for (int i = 0; i < num_tops_; i++) {
        top_chunk_ids_.push_back(
          data_manager_->CreateData(top_size));
        tops_.push_back(
          data_manager_->GetData(top_chunk_ids_[i]));
        top_diff_chunk_ids_.push_back(
          data_manager_->CreateData(top_size));
        top_diffs_.push_back(
          data_manager_->GetData(top_diff_chunk_ids_[i]));
      }

    }

    // Only one set of weights is considered

    int weights_size = conv_param_.output_num_ *
                       input_dim_.c_ *
                       conv_param_.kernel_size_h_ *
                       conv_param_.kernel_size_w_;
    weights_chunk_id_ = data_manager_->CreateData(weights_size);
    weights_ = data_manager_->GetData(weights_chunk_id_);
    weights_diff_chunk_id_ =
      data_manager_->CreateData(weights_size);
    weights_diff_ = data_manager_->GetData(weights_diff_chunk_id_);

    // Fill the weight data
    weights_->Filler();

#ifdef NVIDIA_CUDNN
    // Set convolution forward algorithm
    if (conv_param_.algofwd_  == "cudnn" ) {
      if (conv_param_.workspace_size <=1 ) {
        conv_algo_.SetFwdAlgo(*(p_dnnmark_->GetHandle()),
                    p_dnnmark_->getRunMode(), layer_id_,
                    bottom_desc_,
                    desc_,
                    top_desc_,
                    conv_param_.conv_fwd_pref_);
      } else {
        conv_algo_.SetFwdAlgo(*(p_dnnmark_->GetHandle()),
                    p_dnnmark_->getRunMode(), layer_id_,
                    bottom_desc_,
                    desc_,
                    top_desc_,
                    conv_param_.conv_fwd_pref_,
                    conv_param_.workspace_size);
        LOG(INFO) << "Provided workspace size " << conv_param_.workspace_size;
      }
      LOG(INFO) << "Set cuDNN recommended FWD conv. algo: " << conv_algo_.GetFwdAlgo();
    } else if (conv_param_.algofwd_ == "auto" ) {
      // Query cuDNN for the fastest FWD convolution algorithm.
      // Use cuDNN function cudnnFindConvolutionBackwardFilterAlgorithm (called inside FindBwdFilterAlgo())
      // NOTE: The below code selects algorithms prior to run, during setup phase.
      LOG(INFO) << "Performing fastest FWD conv. algo search.\n";
      conv_algo_.FindFwdAlgo(*(p_dnnmark_->GetHandle()),
                                       p_dnnmark_->getRunMode(), layer_id_,
                                       bottom_desc_,
                                       desc_,
                                       top_desc_);
      LOG(INFO) << "cuDNN fastest FWD conv. algo:" << conv_algo_.GetFwdAlgo();
    } else if (conv_param_.algofwd_ == "cudnnv7" ) {
      // Query cuDNN for FWD convolution algorithms.
      // Uses cuDNN function cudnnGetConvolutionForwardAlgorithm_v7 (called inside GetFwdAlgo_v7())
      LOG(INFO) << "Get fastest FWD conv. algorithms.\n";
      conv_algo_.GetFwdAlgo_v7(*(p_dnnmark_->GetHandle()),
                                       p_dnnmark_->getRunMode(), layer_id_,
                                       bottom_desc_,
                                       desc_,
                                       top_desc_);
      LOG(INFO) << "cuDNN recommended FWD conv. algo: " << conv_algo_.GetFwdAlgo();
    } else if (conv_param_.algofwd_  != "" ) {
      conv_algo_.SetFwdAlgo(conv_param_.algofwd_);
    }

    LOG(INFO) << "FWD conv. algo: " << conv_algo_.GetFwdAlgo();

    // Allocate workspace
    conv_algo_.GetFwdWorkspaceSize(*(p_dnnmark_->GetHandle()),
                                   p_dnnmark_->getRunMode(), layer_id_,
                                   bottom_desc_,
                                   top_desc_,
                                   desc_,
                                   &fwd_workspace_size_);
    if (fwd_workspace_size_ > 0) {
      fwd_workspace_id_ = data_manager_->CreateData(fwd_workspace_size_);
      fwd_workspace_ = data_manager_->GetData(fwd_workspace_id_);
      has_fwd_workspace_ = true;
    }



    // Allocate workspace in case workspace_size provided
    if (conv_param_.workspace_size > 0 ) {
      LOG(INFO) << "Allocating BWD Filter workspace of size " << conv_param_.workspace_size;
      bwd_filter_workspace_id_ = data_manager_->
                                 CreateData(conv_param_.workspace_size);
      bwd_filter_workspace_ = data_manager_->GetData(bwd_filter_workspace_id_);
      has_bwd_filter_workspace_ = true;
      bwd_filter_workspace_size_ = conv_param_.workspace_size;
    }

    // Set convolution backward filter/weights algorithm
    if (conv_param_.algo_ == "cudnn") {
      // Chainer default behaviour
      // Use cuDNN function cudnnGetConvolutionBackwardFilterAlgorithm
      if (conv_param_.workspace_size <=1 ) {
        conv_algo_.SetBwdFilterAlgo(*(p_dnnmark_->GetHandle()),
                                         p_dnnmark_->getRunMode(), layer_id_,
                                         bottom_desc_,
                                         top_desc_,
                                         desc_,
                                         conv_param_.conv_bwd_filter_pref_);
      } else {
        conv_algo_.SetBwdFilterAlgo(*(p_dnnmark_->GetHandle()),
                                         p_dnnmark_->getRunMode(), layer_id_,
                                         bottom_desc_,
                                         top_desc_,
                                         desc_,
                                         conv_param_.conv_bwd_filter_pref_,
                                         conv_param_.workspace_size);
        LOG(INFO) << "Provided workspace size " << conv_param_.workspace_size;
      }
      LOG(INFO) << "Set cuDNN recommended BWD conv. Filter algo to " << conv_algo_.GetBwdFilterAlgo();
    } else if (conv_param_.algo_ == "cudnnv7" ) {
      // Query cuDNN for BWD convolution filter gradient algorithm.
      // Use cuDNN function cudnnGetConvolutionBackwardFilterAlgorithm_v7

      LOG(INFO) << "Get fastest BWD conv. filter algorithms.\n";
      conv_algo_.GetdBwdFilterAlgo_v7(*(p_dnnmark_->GetHandle()),
                                         p_dnnmark_->getRunMode(), layer_id_,
                                         bottom_desc_,
                                         desc_,
                                         top_desc_);
      LOG(INFO) << "cuDNN recommended BWD conv. filter algo:" << conv_algo_.GetBwdFilterAlgo();
    } else if (conv_param_.algo_ == "auto" ) {
      // Query cuDNN for the fastest BWD convolution filter gradient algorithm.
      // Use cuDNN function cudnnFindConvolutionBackwardFilterAlgorithm (called inside FindBwdFilterAlgo())

      // NOTE: The below code selects algorithms prior to run, during setup phase.
      LOG(INFO) << "Performing fastest BWD conv. filter algo search.\n";
      if (conv_param_.workspace_size <=1 ) {
        // No workspace size limit provided
        conv_algo_.FindBwdFilterAlgo(*(p_dnnmark_->GetHandle()),
                                         p_dnnmark_->getRunMode(), layer_id_,
                                         bottom_desc_,
                                         desc_,
                                         top_desc_);
      } else {
        // Search for fastest algorithms using Find...AlgoEX() function
        LOG(INFO) << (has_bwd_filter_workspace_? "Has BWD filter workspace. " : "No BWD filter workspace.") << "Provided workspace size " << conv_param_.workspace_size;

        conv_algo_.FindBwdFilterAlgoEx(*(p_dnnmark_->GetHandle()),
                                         p_dnnmark_->getRunMode(), layer_id_,
                                         bottom_desc_,
                                         desc_,
                                         top_desc_,
                                         bottoms_[0]->Get(),
                                         top_diffs_[0]->Get(),
                                         weights_diff_->Get(),
                                         has_bwd_filter_workspace_? bwd_filter_workspace_->Get() : nullptr,
                                         conv_param_.workspace_size);
      }
      LOG(INFO) << "cuDNN fastest BWD conv. filter algo:" << conv_algo_.GetBwdFilterAlgo();
    } else if (conv_param_.algo_ != "") {
        // Use default algorithm for now
        LOG(INFO) << "Setting Bwd Filter algo to " << conv_param_.algo_;
        conv_algo_.SetBwdFilterAlgo(conv_param_.algo_);
    }
#endif
#ifdef AMD_MIOPEN
    // Use default algorithm for now
    LOG(INFO) << "Setting BWD Filter algo to " << conv_param_.algo_;
    conv_algo_.SetBwdFilterAlgo(conv_param_.algo_);
#endif

    LOG(INFO) << "BWD conv. Filter algo: " << conv_algo_.GetBwdFilterAlgo();

    // Allocate workspace if it has not been allocated yet
    if (conv_param_.workspace_size <= 1 ) {
      conv_algo_.GetBwdFilterWorkspaceSize(*(p_dnnmark_->GetHandle()),
                                           p_dnnmark_->getRunMode(), layer_id_,
                                           bottom_desc_,
                                           top_desc_,
                                           desc_,
                                           &bwd_filter_workspace_size_);
      LOG(INFO) << "BWD Filter workspace size: " << bwd_filter_workspace_size_;
      if (bwd_filter_workspace_size_ > 0) {
        bwd_filter_workspace_id_ = data_manager_->
                                   CreateData(bwd_filter_workspace_size_);
        bwd_filter_workspace_ = data_manager_->GetData(bwd_filter_workspace_id_);
        has_bwd_filter_workspace_ = true;
      }
    } else {
      LOG(INFO) << "BWD Filter workspace size: " << bwd_filter_workspace_size_;
    }

#ifdef NVIDIA_CUDNN
    // Set convolution backward data algorithm
    if (conv_param_.algod_ == "cudnn") {
      // Use cuDNN function cudnnGetConvolutionBackwardFilterAlgorithm
      if (conv_param_.workspace_size <=1 ) {
        conv_algo_.SetBwdDataAlgo(*(p_dnnmark_->GetHandle()),
                                         p_dnnmark_->getRunMode(), layer_id_,
                                         bottom_desc_,
                                         top_desc_,
                                         desc_,
                                         conv_param_.conv_bwd_data_pref_);
      } else {
        conv_algo_.SetBwdDataAlgo(*(p_dnnmark_->GetHandle()),
                                         p_dnnmark_->getRunMode(), layer_id_,
                                         bottom_desc_,
                                         top_desc_,
                                         desc_,
                                         conv_param_.conv_bwd_data_pref_,
                                         conv_param_.workspace_size);
        LOG(INFO) << "Provided workspace size " << conv_param_.workspace_size;
      }
      LOG(INFO) << "Set cuDNN recommended BWD conv. data algo to " << conv_algo_.GetBwdDataAlgo();
    } else if (conv_param_.algod_ == "cudnnv7" ) {
        // Query cuDNN for BWD convolution data gradient algorithm.
        // Use cuDNN function cudnnGetConvolutionBackwardDataAlgorithm_v7 (called inside FindBwdDataAlgo())

        LOG(INFO) << "Get fastest BWD conv. data algorithms.\n";
        conv_algo_.GetBwdDataAlgo_v7(*(p_dnnmark_->GetHandle()),
                                         p_dnnmark_->getRunMode(), layer_id_,
                                         bottom_desc_,
                                         desc_,
                                         top_desc_);
        LOG(INFO) << "cuDNN recommended BWD conv. data algo:" << conv_algo_.GetBwdDataAlgo();
    } else if (conv_param_.algod_ == "auto" ) {
        // Query cuDNN for the fastest BWD convolution data gradient algorithm.
        // Use cuDNN function cudnnFindConvolutionBackwardDataAlgorithm (called inside FindBwdDataAlgo())
        // NOTE: The below code selects algorithms prior to run, during setup phase.

        LOG(INFO) << "Performing fastest BWD conv. data algo search.\n";
        conv_algo_.FindBwdDataAlgo(*(p_dnnmark_->GetHandle()),
                                         p_dnnmark_->getRunMode(), layer_id_,
                                         bottom_desc_,
                                         desc_,
                                         top_desc_);
        LOG(INFO) << "cuDNN fastest BWD conv. data algo:" << conv_algo_.GetBwdDataAlgo();
    } else if (conv_param_.algod_ != "") {
      conv_algo_.SetBwdDataAlgo(conv_param_.algod_);
    }
#endif

#ifdef AMD_MIOPEN
    conv_algo_.SetBwdDataAlgo(conv_param_.algod_);
#endif

    LOG(INFO) << "BWD conv. data algo: "<< static_cast<int>(conv_algo_.GetBwdDataAlgo());

    // Allocate workspace
    conv_algo_.GetBwdDataWorkspaceSize(*(p_dnnmark_->GetHandle()),
                                       p_dnnmark_->getRunMode(), layer_id_,
                                       bottom_desc_,
                                       top_desc_,
                                       desc_,
                                       &bwd_data_workspace_size_);
    if (bwd_data_workspace_size_ > 0) {
      bwd_data_workspace_id_ = data_manager_->
                                 CreateData(bwd_data_workspace_size_);
      bwd_data_workspace_ = data_manager_->GetData(bwd_data_workspace_id_);
      has_bwd_data_workspace_ = true;
    }

  }

  void ComputeOutputDim() {
    output_dim_.n_ = input_dim_.n_;
    output_dim_.c_ = conv_param_.output_num_;
    output_dim_.h_ = (input_dim_.h_ +
      2 * conv_param_.pad_h_ - conv_param_.kernel_size_h_) /
      conv_param_.stride_u_ + 1;
    output_dim_.w_ = (input_dim_.w_ +
      2 * conv_param_.pad_w_ - conv_param_.kernel_size_w_) /
      conv_param_.stride_v_ + 1;
  }

  void ForwardPropagation() {
    // Fill the bottom data
    if (p_dnnmark_->getRunMode() == STANDALONE ||
        !previous_layer_name_.compare("null")) {
      for (int i = 0; i < num_bottoms_; i++) {
        bottoms_[i]->Filler();
      }
    }
    // Convolution forward computation
    for (int i = 0; i < num_bottoms_; i++) {
      LOG(INFO) << "Calling dnnmarkConvolutionForward";
      LOG(INFO) << "workspace size" << fwd_workspace_size_;
      dnnmarkConvolutionForward(
                *(p_dnnmark_->GetHandle()),
                p_dnnmark_->getRunMode(), layer_id_,
                p_dnnmark_->GetTimer(),
                DataType<T>::one,
                bottom_desc_, bottoms_[i]->Get(),
                desc_, weights_->Get(),
                &conv_algo_,
                has_fwd_workspace_? fwd_workspace_->Get() : nullptr,
                fwd_workspace_size_,
                DataType<T>::zero,
                top_desc_, tops_[i]->Get());
    }
  }

  void BackwardPropagation() {
    if (p_dnnmark_->getRunMode() == STANDALONE ||
        !previous_layer_name_.compare("null")) {
      // Fill the top data and top diff data
      for (int i = 0; i < num_tops_; i++) {
        tops_[i]->Filler();
        top_diffs_[i]->Filler();
      }
      // Fill the bottom data
      for (int i = 0; i < num_bottoms_; i++) {
        bottoms_[i]->Filler();
      }
    }

    LOG(INFO) << "Performnig BWD convolution. " << (has_bwd_filter_workspace_? "Has BWD filter workspace. " : "No BWD filter workspace.") << " Provided workspace size " << bwd_filter_workspace_size_;


    // Convolution backward computation
    for (int i = 0; i < num_tops_; i++) {
      dnnmarkConvolutionBackwardFilter(
                  *(p_dnnmark_->GetHandle()),
                  p_dnnmark_->getRunMode(), layer_id_,
                  p_dnnmark_->GetTimer(),
                  DataType<T>::one,
                  bottom_desc_, bottoms_[i]->Get(),
                  top_desc_, top_diffs_[i]->Get(),
                  desc_,
                  &conv_algo_,
                  has_bwd_filter_workspace_? bwd_filter_workspace_->Get() : nullptr,
                  bwd_filter_workspace_size_,
                  DataType<T>::zero,
                  weights_diff_->Get());
      if (conv_param_.propagation_) {
        dnnmarkConvolutionBackwardData(
                  *(p_dnnmark_->GetHandle()),
                  p_dnnmark_->getRunMode(), layer_id_,
                  p_dnnmark_->GetTimer(),
                  DataType<T>::one,
                  top_desc_, top_diffs_[i]->Get(),
                  desc_, weights_->Get(),
                  &conv_algo_,
                  has_bwd_data_workspace_? bwd_data_workspace_->Get() : nullptr,
                  bwd_data_workspace_size_,
                  DataType<T>::zero,
                  bottom_desc_, bottoms_[i]->Get());
      }
    }
  }

};

} // namespace dnnmark

#endif // CORE_INCLUDE_LAYERS_CONV_LAYER_H_
