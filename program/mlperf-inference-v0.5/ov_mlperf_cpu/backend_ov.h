#ifndef BACKENDOV_H__
#define BACKENDOV_H__

#include <inference_engine.hpp>
#include <list.hpp>

#include "infer_request_wrap.h"

using namespace InferenceEngine;

class BackendOV {
public:
    BackendOV(mlperf::TestSettings settings, unsigned nireq, int nstreams,
            int batch_size, int nthreads, std::string dataset,
            std::string workload) {
        this->settings_ = settings;
        this->nireq_ = nireq;
        this->batch_size_ = batch_size;
        this->nstreams_ = nstreams;
        this->nthreads_ = nthreads;
        this->dataset_ = dataset;
        this->workload_ = workload;
    }

    ~BackendOV() {
    }

    std::string version() {
        return "";
    }

    std::string name() {
        return "openvino";
    }

    std::string image_format() {
        // Find right format
        return "NCHW";
    }

    void setBatchSize(size_t batch_size) {
        network_reader_.getNetwork().setBatchSize(batch_size);
    }

    std::string fileNameNoExt(const std::string &filepath) {
        auto pos = filepath.rfind('.');
        if (pos == std::string::npos)
            return filepath;
        return filepath.substr(0, pos);
    }

    void load(const std::string input_model) {
        // Read IR generated by ModelOptimizer - .xml and .bin files
        network_reader_.ReadNetwork(input_model);
        network_reader_.ReadWeights(fileNameNoExt(input_model) + ".bin");
        network_ = network_reader_.getNetwork();

        // Configure input/output
        InputsDataMap inputInfo(network_.getInputsInfo());
        input_name_ = network_.getInputsInfo().begin()->first;
        auto inputInfoItem = *inputInfo.begin();
        inputInfoItem.second->setPrecision(Precision::FP32);
        inputInfoItem.second->setLayout(Layout::NCHW);

        DataPtr output_info = network_.getOutputsInfo().begin()->second;
        output_name_ = network_.getOutputsInfo().begin()->first;
        output_info->setPrecision(Precision::FP32);

        const SizeVector outputDims = output_info->getTensorDesc().getDims();
        if (workload_.compare("ssd-mobilenet") == 0) {
            max_proposal_count_ = outputDims[2];
            object_size_ = outputDims[3];
        } else if (workload_.compare("ssd-resnet34") == 0) {
            max_proposal_count_ = outputDims[1];
            object_size_ = outputDims[2];
        }

        // Load model to device
        Core ie;
        const std::string device { "CPU" };
        if (device == "CPU") {
            ie.AddExtension(std::make_shared<Extensions::Cpu::MKLDNNExtensions>(),
                    "CPU");
            if (settings_.scenario == mlperf::TestScenario::SingleStream) {
                ie.SetConfig(
                        { { CONFIG_KEY(CPU_THREADS_NUM), std::to_string(
                                this->nthreads_) } }, device);
                ie.SetConfig( {
                        { CONFIG_KEY(CPU_BIND_THREAD), CONFIG_VALUE(YES) } },
                        device);
            }
            if (settings_.scenario == mlperf::TestScenario::Offline) {
                ie.SetConfig( { { CONFIG_KEY(CPU_THROUGHPUT_STREAMS),
                        std::to_string(this->nstreams_) } }, device);

                ie.SetConfig(
                        { { CONFIG_KEY(CPU_THREADS_NUM), std::to_string(
                                this->nthreads_) } }, device);
                ie.SetConfig( {
                        { CONFIG_KEY(CPU_BIND_THREAD), CONFIG_VALUE(YES) } },
                        device);
            }
            if (settings_.scenario == mlperf::TestScenario::Server) {
                ie.SetConfig( { { CONFIG_KEY(CPU_THROUGHPUT_STREAMS),
                        std::to_string(this->nstreams_) } }, device);
                ie.SetConfig(
                        { { CONFIG_KEY(CPU_THREADS_NUM), std::to_string(
                                this->nthreads_) } }, device);
                ie.SetConfig( {
                        { CONFIG_KEY(CPU_BIND_THREAD), CONFIG_VALUE(YES) } },
                        device);
            }
            if (settings_.scenario == mlperf::TestScenario::MultiStream) {
                ie.SetConfig( { { CONFIG_KEY(CPU_THROUGHPUT_STREAMS),
                        std::to_string(this->nstreams_) } }, device);
                ie.SetConfig(
                        { { CONFIG_KEY(CPU_THREADS_NUM), std::to_string(
                                this->nthreads_) } }, device);
                ie.SetConfig( {
                        { CONFIG_KEY(CPU_BIND_THREAD), CONFIG_VALUE(YES) } },
                        device);
            }
        }

        if (workload_.compare("resnet50") == 0) {
            setBatchSize(batch_size_);
        } else if(!(workload_.compare("ssd-resnet34") == 0)){
            auto input_shapes = network_.getInputShapes();
            std::string input_name;
            SizeVector input_shape;
            std::tie(input_name, input_shape) = *input_shapes.begin();
            input_shape[0] = batch_size_;
            input_shapes[input_name] = input_shape;
            network_.reshape(input_shapes);
        }

        std::map < std::string, std::string > config;
        exe_network_ = ie.LoadNetwork(network_, device, config);

        if (settings_.scenario == mlperf::TestScenario::SingleStream) {
            inferRequest_ = exe_network_.CreateInferRequest();
        } else if (settings_.scenario == mlperf::TestScenario::Offline) {

            inferRequestsQueue_ = new InferRequestsQueue(exe_network_, nireq_,
                    output_name_, settings_, workload_);
        } else if (settings_.scenario == mlperf::TestScenario::Server) {
            inferRequestsQueue_ = new InferRequestsQueue(exe_network_, nireq_,
                    output_name_, settings_, workload_);
        } else if (settings_.scenario == mlperf::TestScenario::MultiStream) {
            inferRequestsQueue_ = new InferRequestsQueue(exe_network_, nireq_,
                    output_name_, settings_, workload_);
        }
    }

    void reset() {
        inferRequestsQueue_->reset();
    }

    void postProcessSSDMobilenet(std::vector<Item> outputs,
            std::vector<float> &results, std::vector<unsigned> &counts,
            std::vector<mlperf::ResponseId> &response_ids,
            unsigned num_batches) {
        unsigned count = 0;
        int image_id = 0, prev_image_id = 0;
        size_t j = 0;

        for (size_t i = 0; i < num_batches; ++i) {
            Blob::Ptr out = outputs[i].blob_;
            std::vector < mlperf::QuerySampleIndex > sample_idxs =
                    outputs[i].sample_idxs_;
            std::vector < mlperf::QuerySampleIndex > resp_ids =
                    outputs[i].response_ids_;

            const float* detection =
                    static_cast<PrecisionTrait<Precision::FP32>::value_type*>(out->buffer());

            count = 0;
            image_id = 0;
            prev_image_id = 0;
            j = 0;
            for (int curProposal = 0; curProposal < max_proposal_count_;
                    curProposal++) {
                image_id = static_cast<int>(detection[curProposal * object_size_
                        + 0]);

                if (image_id != prev_image_id) {
                    counts.push_back(count * 7);
                    response_ids.push_back(resp_ids[j]);
                    ++j;
                    count = 0;
                    prev_image_id = prev_image_id + 1;
                    if (image_id > 0) {
                        while (image_id != prev_image_id) {
                            counts.push_back(count * 7);
                            response_ids.push_back(resp_ids[j]);
                            ++j;
                            count = 0;
                            prev_image_id = prev_image_id + 1;
                        }
                    } else {
                        while (prev_image_id < (int) batch_size_) {
                            counts.push_back(count * 7);
                            response_ids.push_back(resp_ids[j]);
                            ++j;
                            count = 0;
                            prev_image_id = prev_image_id + 1;
                        }
                    }
                }
                if (image_id < 0) {
                    break;
                }

                float confidence = detection[curProposal * object_size_ + 2];
                float label = static_cast<float>(detection[curProposal
                        * object_size_ + 1]);
                float xmin = static_cast<float>(detection[curProposal
                        * object_size_ + 3]);
                float ymin = static_cast<float>(detection[curProposal
                        * object_size_ + 4]);
                float xmax = static_cast<float>(detection[curProposal
                        * object_size_ + 5]);
                float ymax = static_cast<float>(detection[curProposal
                        * object_size_ + 6]);

                if (confidence > 0.05) {
                    /** Add only objects with >95% probability **/
                    results.push_back(float(sample_idxs[j]));
                    results.push_back(ymin);
                    results.push_back(xmin);
                    results.push_back(ymax);
                    results.push_back(xmax);
                    results.push_back(confidence);
                    results.push_back(label);

                    ++count;
                }

                if (curProposal == (max_proposal_count_ - 1)) {
                    counts.push_back(count * 7);
                    response_ids.push_back(resp_ids[j]);
                    ++j;
                    count = 0;
                    prev_image_id = prev_image_id + 1;
                    while (prev_image_id < (int) batch_size_) {
                        counts.push_back(count * 7);
                        response_ids.push_back(resp_ids[j]);
                        ++j;
                        count = 0;
                        prev_image_id = prev_image_id + 1;
                    }
                }
            }
        }
    }

    void postProcessSSDResnet(std::vector<Item> outputs,
            std::vector<float> &result, std::vector<unsigned> &counts,
            std::vector<mlperf::ResponseId> &response_ids,
            unsigned num_batches) {
        unsigned count = 0;
        for (size_t i = 0; i < num_batches; ++i) {
            Blob::Ptr bbox_blob = outputs[i].blob_;
            Blob::Ptr scores_blob = outputs[i].blob1_;
            Blob::Ptr labels_blob = outputs[i].blob2_;
            std::vector < mlperf::QuerySampleIndex > sample_idxs =
                    outputs[i].sample_idxs_;

            const float* BoundingBoxes =
                    static_cast<float*>(bbox_blob->buffer());
            const float* Confidence = static_cast<float*>(scores_blob->buffer());
            const float* Labels = static_cast<float*>(labels_blob->buffer());

            for (size_t j = 0; j < batch_size_; ++j) {
                auto cur_item = (j * max_proposal_count_);
                auto cur_bbox = (j * max_proposal_count_ * object_size_);

                count = 0;
                for (int curProposal = 0; curProposal < max_proposal_count_;
                        curProposal++) {
                    float confidence = Confidence[cur_item + curProposal];
                    float label =
                            static_cast<int>(Labels[cur_item + curProposal]);
                    float xmin = static_cast<float>(BoundingBoxes[cur_bbox
                            + curProposal * object_size_ + 0]);
                    float ymin = static_cast<float>(BoundingBoxes[cur_bbox
                            + curProposal * object_size_ + 1]);
                    float xmax = static_cast<float>(BoundingBoxes[cur_bbox
                            + curProposal * object_size_ + 2]);
                    float ymax = static_cast<float>(BoundingBoxes[cur_bbox
                            + curProposal * object_size_ + 3]);

                    if (confidence > 0.05) {
                        /** Add only objects with > 0.05 probability **/
                        result.push_back(float(sample_idxs[j]));
                        result.push_back(ymin);
                        result.push_back(xmin);
                        result.push_back(ymax);
                        result.push_back(xmax);
                        result.push_back(confidence);
                        result.push_back(label);

                        ++count;
                    }
                }

                counts.push_back(count * 7);
                response_ids.push_back(outputs[i].response_ids_[j]);
            }
        }
    }

    void postProcessImagenet(std::vector<Item> &blob,
            std::vector<unsigned> &results,
            std::vector<mlperf::ResponseId> &response_ids) {

        for (size_t i = 0; i < blob.size(); ++i) {
            Item b = blob[i];
            std::vector<unsigned> res;
            TopResults(1, *(b.blob_), res);

            for (size_t j = 0; j < res.size(); ++j) {
                results.push_back(res[j] - 1);
                response_ids.push_back(b.response_ids_[j]);
            }
        }
    }

    void predict(Blob::Ptr input, std::vector<float> &result,
            mlperf::QuerySampleIndex sample_id, mlperf::ResponseId response_id,
            std::vector<unsigned> &counts) {

        std::vector < mlperf::QuerySampleIndex > sample_idxs;
        sample_idxs.push_back(sample_id);
        std::vector < mlperf::ResponseId > response_ids;
        response_ids.push_back(response_id);
        inferRequest_.SetBlob(input_name_, input);

        inferRequest_.Infer(); // Synchronous
        std::vector < mlperf::ResponseId > res_ids;
        if (workload_.compare("ssd-mobilenet") == 0) {
            std::vector<Item> outputs;

            outputs.push_back(
                    Item(inferRequest_.GetBlob(output_name_), response_ids, sample_idxs));
            postProcessSSDMobilenet(outputs, result, counts, res_ids, 1);
        } else if (workload_.compare("ssd-resnet34") == 0) {
            std::vector<Item> outputs;

            Item item;
            item.blob_ = inferRequest_.GetBlob("Unsqueeze_bboxes777");
            item.blob1_ = inferRequest_.GetBlob("Unsqueeze_scores835");
            item.blob2_ = inferRequest_.GetBlob("Add_labels");
            item.sample_idxs_ = sample_idxs;
            item.response_ids_ = response_ids;

            outputs.push_back(item);

            postProcessSSDResnet(outputs, result, counts, res_ids, 1);
        }

        return;
    }

    void predict(Blob::Ptr input, int label, unsigned &result) {
        inferRequest_.SetBlob(input_name_, input);

        inferRequest_.Infer(); // Synchronous

        Blob::Ptr output = inferRequest_.GetBlob(output_name_);
        std::vector<unsigned> results;
        TopResults(1, *output, results);
        result = results[0] - 1;

        return;
    }

    void predictOffline(std::vector<Item> inputs,
            std::vector<unsigned> &results,
            std::vector<mlperf::ResponseId> &response_ids,
            unsigned num_batches) {

        size_t i = 0;

        for (i = 0; i < num_batches; ++i) {
            auto inferRequest = inferRequestsQueue_->getIdleRequest();

            inferRequest->setInputs(inputs[i], input_name_);

            inferRequest->startAsync();
        }

        inferRequestsQueue_->waitAll();

        auto res = inferRequestsQueue_->getOutputs();

        if ((workload_.compare("resnet50") == 0)
                || (workload_.compare("mobilenet") == 0)) {
            postProcessImagenet(res, results, response_ids);
        }

        return;
    }

    void predictOffline(std::vector<Item> inputs, std::vector<float> &results,
            std::vector<mlperf::ResponseId> &response_ids,
            std::vector<mlperf::QuerySampleIndex> &sample_idxs,
            std::vector<unsigned> &counts, unsigned num_batches) {
        size_t i = 0;

        for (i = 0; i < num_batches; ++i) {
            auto inferRequest = inferRequestsQueue_->getIdleRequest();

            inferRequest->setInputs(inputs[i], input_name_);

            inferRequest->startAsync();
        }

        inferRequestsQueue_->waitAll();

        auto res = inferRequestsQueue_->getOutputs();

        if (workload_.compare("ssd-resnet34") == 0) {
            postProcessSSDResnet(res, results, counts, response_ids,
                    num_batches);
        } else if (workload_.compare("ssd-mobilenet") == 0) {
            postProcessSSDMobilenet(res, results, counts, response_ids,
                    num_batches);
        }

        return;
    }

    void predictServer(Item input, bool is_warm_up = false) {
        auto inferRequest = inferRequestsQueue_->getIdleRequest();

        inferRequest->setIsWarmup(is_warm_up);

        inferRequest->setInputs(input, input_name_);

        inferRequest->startAsync();

        return;
    }

    void predictMultiStream(std::vector<Item> inputs,
            std::vector<unsigned> &results,
            std::vector<mlperf::ResponseId> &response_ids,
            unsigned num_batches) {
        std::vector < Blob::Ptr > outputBlobs_;
        size_t i = 0;

        for (i = 0; i < num_batches; ++i) {
            auto inferRequest = inferRequestsQueue_->getIdleRequest();
            inferRequest->setInputs(inputs[i], input_name_);

            inferRequest->startAsync();
        }

        inferRequestsQueue_->waitAll();
        auto res = inferRequestsQueue_->getOutputs();

        if ((workload_.compare("resnet50") == 0)
                || (workload_.compare("mobilenet") == 0)) {
            postProcessImagenet(res, results, response_ids);
        }
        return;
    }

    void predictMultiStream(std::vector<Item> inputs,
            std::vector<float> &results,
            std::vector<mlperf::ResponseId> &response_ids,
            std::vector<mlperf::QuerySampleIndex> &sample_idxs,
            std::vector<unsigned> &counts, unsigned num_batches) {
        size_t i = 0;

        for (i = 0; i < num_batches; ++i) {
            auto inferRequest = inferRequestsQueue_->getIdleRequest();
            inferRequest->setInputs(inputs[i], input_name_);

            inferRequest->startAsync();
        }

        inferRequestsQueue_->waitAll();
        auto res = inferRequestsQueue_->getOutputs();

        if (workload_.compare("ssd-resnet34") == 0) {
            postProcessSSDResnet(res, results, counts, response_ids,
                    num_batches);

        } else if (workload_.compare("ssd-mobilenet") == 0) {
            postProcessSSDMobilenet(res, results, counts, response_ids,
                    num_batches);
        }

        return;
    }

    unsigned getResult(Blob::Ptr output) {
        std::vector<unsigned> results;

        TopResults(1, *output, results);

        return results[0];
    }

private:
    CNNNetReader network_reader_;
    CNNNetwork network_;
    ExecutableNetwork exe_network_;
    mlperf::TestSettings settings_;
    unsigned batch_size_ = 1;
    int nstreams_ = 1;
    unsigned nireq_ = 1;
    int nthreads_ = 56;
    std::vector<Blob::Ptr> outputBlobs_;
    InferRequestsQueue* inferRequestsQueue_; // Offline, Server, MultiStream
    std::string input_name_;
    std::string output_name_;
    InferRequest inferRequest_; // SingleStream
    std::string dataset_;
    int max_proposal_count_;
    std::string workload_;
    int object_size_;
};

#endif
