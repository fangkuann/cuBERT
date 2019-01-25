#include "cuBERT/common.h"

#include "Bert.h"


namespace cuBERT {
    Bert::Bert(const std::unordered_map<std::string, float *> &var,
               size_t max_batch_size,
               size_t seq_length,
               size_t vocab_size,
               size_t type_vocab_size,
               size_t hidden_size,
               size_t num_hidden_layers,
               size_t num_attention_heads,
               size_t intermediate_size) {
        this->seq_length = seq_length;
        this->hidden_size = hidden_size;

        CUDA_CHECK(cudaStreamCreate(&this->stream));
        CUBLAS_CHECK(cublasCreate_v2(&this->cublas));
        CUDNN_CHECK(cudnnCreate(&this->cudnn));
        CUBLAS_CHECK(cublasSetStream_v2(cublas, stream));
        CUDNN_CHECK(cudnnSetStream(cudnn, stream));

        this->bert_embeddings = new BertEmbeddings(cublas, var, max_batch_size,
                                                   vocab_size, type_vocab_size, hidden_size, seq_length);

        this->transformer = new Transformer(cublas, cudnn, "bert/encoder", var,
                                            max_batch_size, seq_length,
                                            hidden_size, num_hidden_layers, num_attention_heads, intermediate_size);

        this->bert_pooler = new BertPooler(cublas, seq_length, hidden_size,
                                           var.at("bert/pooler/dense/kernel"),
                                           var.at("bert/pooler/dense/bias"),
                                           max_batch_size);

        this->additional_output_layer = new AdditionalOutputLayer(cublas, hidden_size, var.at("output_weights"));

        CUDA_CHECK(cudaMalloc(&this->embedding_output_gpu, sizeof(float) * max_batch_size * seq_length * hidden_size));
        CUDA_CHECK(cudaMalloc(&this->pooled_output_gpu, sizeof(float) * max_batch_size * hidden_size));

        CUDA_CHECK(cudaMalloc(&this->logits_gpu, sizeof(float) * max_batch_size));

        CUDA_CHECK(cudaMalloc(&this->input_ids_gpu, sizeof(int) * max_batch_size * seq_length));
        CUDA_CHECK(cudaMalloc(&this->input_mask_gpu, sizeof(char) * max_batch_size * seq_length));
        CUDA_CHECK(cudaMalloc(&this->segment_ids_gpu, sizeof(char) * max_batch_size * seq_length));
    }

    Bert::~Bert() {
        CUDA_CHECK(cudaFree(segment_ids_gpu));
        CUDA_CHECK(cudaFree(input_mask_gpu));
        CUDA_CHECK(cudaFree(input_ids_gpu));

        CUDA_CHECK(cudaFree(logits_gpu));

        CUDA_CHECK(cudaFree(pooled_output_gpu));
        CUDA_CHECK(cudaFree(embedding_output_gpu));

        delete additional_output_layer;
        delete bert_pooler;
        delete transformer;
        delete bert_embeddings;

        CUDNN_CHECK(cudnnDestroy(cudnn));
        CUBLAS_CHECK(cublasDestroy_v2(cublas));
        CUDA_CHECK(cudaStreamDestroy(stream));
    }

    void Bert::compute_cpu(size_t batch_size, int *input_ids, char *input_mask, char *segment_ids) {
        // copy inputs
        cudaStream_t streamId = nullptr;
        CUBLAS_CHECK(cublasGetStream_v2(cublas, &streamId));

        CUDA_CHECK(cudaMemcpyAsync(input_ids_gpu, input_ids, sizeof(int) * batch_size * seq_length, cudaMemcpyHostToDevice, streamId);
        CUDA_CHECK(cudaMemcpyAsync(input_mask_gpu, input_mask, sizeof(char) * batch_size * seq_length, cudaMemcpyHostToDevice, streamId);
        CUDA_CHECK(cudaMemcpyAsync(segment_ids_gpu, segment_ids, sizeof(char) * batch_size * seq_length, cudaMemcpyHostToDevice, streamId);

        // bert/embeddings
        bert_embeddings->compute(batch_size, input_ids_gpu, segment_ids_gpu, embedding_output_gpu);

        // bert/encoder
        sequence_output_gpu = transformer->compute(batch_size, embedding_output_gpu, input_mask_gpu);

        // bert/pooler
        bert_pooler->compute(batch_size, sequence_output_gpu, pooled_output_gpu);

        additional_output_layer->compute(batch_size, pooled_output_gpu, logits_gpu);
    }

    void Bert::logits(size_t batch_size, float *logits) {
        cudaStream_t streamId = nullptr;
        CUBLAS_CHECK(cublasGetStream_v2(cublas, &streamId));

        CUDA_CHECK(cudaMemcpyAsync(logits, logits_gpu, sizeof(float) * batch_size, cudaMemcpyDeviceToHost, streamId));
        CUDA_CHECK(cudaStreamSynchronize(streamId));
    }

    void Bert::embedding_output(size_t batch_size, float *embedding_output) {
        cudaStream_t streamId = nullptr;
        CUBLAS_CHECK(cublasGetStream_v2(cublas, &streamId));

        CUDA_CHECK(cudaMemcpyAsync(embedding_output,
                           embedding_output_gpu,
                           sizeof(float) * batch_size * seq_length * hidden_size,
                           cudaMemcpyDeviceToHost, streamId);
        CUDA_CHECK(cudaStreamSynchronize(streamId));
    }
}
