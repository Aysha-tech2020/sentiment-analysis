#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h> // Include for srand and time
#include <openacc.h>
#include <nvToolsExt.h> // NVTX for profiling annotations

#define MAX_TOKENS 1024
#define NUM_FEATURES 1024

typedef struct {
    char text[MAX_TOKENS];
    int label;
} Post;

// Custom implementation of strlen
int custom_strlen(const char *str) {
    int length = 0;
    while (str[length] != '\0') {
        length++;
    }
    return length;
}

// Helper: Allocate memory and handle failure
void *safe_malloc(size_t size, const char *name) {
    void *ptr = malloc(size);
    if (!ptr) {
        printf("Error: Memory allocation failed for %s.\n", name);
        exit(1);
    }
    return ptr;
}

// Shuffle the dataset to randomize it
void shuffleDataset(Post *dataset, int num_samples) {
    srand(time(NULL));  // Seed for randomness
    for (int i = 0; i < num_samples; i++) {
        int j = rand() % num_samples;
        // Swap elements
        Post temp = dataset[i];
        dataset[i] = dataset[j];
        dataset[j] = temp;
    }
}

// Load dataset from file
int loadDataset(const char *filename, Post **dataset) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Error: Could not open file %s\n", filename);
        exit(1);
    }

    size_t capacity = 1000;
    size_t count = 0;
    *dataset = (Post *)safe_malloc(capacity * sizeof(Post), "dataset");

    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (count >= capacity) {
            capacity *= 2;
            *dataset = (Post *)realloc(*dataset, capacity * sizeof(Post));
            if (!*dataset) {
                printf("Error: Memory reallocation failed.\n");
                fclose(file);
                exit(1);
            }
        }

        // Read label (first column of dataset)
        char *token = strtok(line, ",");
        if (!token) continue;

        (*dataset)[count].label = atoi(token);  // Use numeric labels (0 for negative, 4 for positive)

        // Skip unnecessary columns (3rd, 4th, and 5th columns)
        token = strtok(NULL, ",");
        for (int i = 0; i < 3; i++) {
            token = strtok(NULL, ",");
        }

        // Read the tweet content (last column)
        token = strtok(NULL, "\n");
        if (token) {
            strncpy((*dataset)[count].text, token, MAX_TOKENS - 1);
            (*dataset)[count].text[MAX_TOKENS - 1] = '\0';
            count++;
        }
    }

    fclose(file);
    return count;
}

// Load and split the dataset into training and testing
int loadAndSplitDataset(const char *filename, Post **trainSet, Post **testSet, int *trainSize, int *testSize) {
    clock_t start_time = clock(); // Start time measurement
    Post *dataset = NULL;
    int num_samples = loadDataset(filename, &dataset);  // Load entire dataset

    // Shuffle the dataset to randomize the order
    shuffleDataset(dataset, num_samples);

    // Split into 70% training and 30% testing
    *trainSize = (int)(num_samples * 0.7);
    *testSize = num_samples - *trainSize;

    // Allocate memory for the training and testing sets
    *trainSet = (Post *)safe_malloc(*trainSize * sizeof(Post), "trainSet");
    *testSet = (Post *)safe_malloc(*testSize * sizeof(Post), "testSet");

    // Copy the data into the train and test sets
    for (int i = 0; i < *trainSize; i++) {
        (*trainSet)[i] = dataset[i];
    }
    for (int i = 0; i < *testSize; i++) {
        (*testSet)[i] = dataset[*trainSize + i];
    }

    free(dataset);  // Free the original dataset after splitting
    clock_t end_time = clock(); // End time measurement
    double execution_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    printf("Loading and Splitting Time: %.4f seconds\n", execution_time);

    return num_samples;
}

// Modified tokenization using hash function (previously used ASCII values)
void tokenizeAndEmbed(Post *dataset, float *token_ids, int num_samples) {
    clock_t start_time = clock(); // Start time measurement
    nvtxRangePush("tokenizeAndEmbed");
    #pragma acc parallel loop present(dataset[:num_samples], token_ids[:num_samples * MAX_TOKENS])
    for (int i = 0; i < num_samples; i++) {
        for (int j = 0; j < custom_strlen(dataset[i].text); j++) {
            token_ids[i * MAX_TOKENS + j] = (float)(dataset[i].text[j]) / 255.0f;
        }
    }
      
    clock_t end_time = clock(); // End time measurement
    double execution_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    printf("Tokenization Time: %.4f seconds\n", execution_time);
    nvtxRangePop();
}

// Random weight initialization using Xavier method
void init_weights(float *weights, int num_features) {
    for (int i = 0; i < num_features * num_features; i++) {
        weights[i] = ((float)rand() / RAND_MAX) * sqrt(2.0f / num_features); // Xavier initialization
    }
}

// Dense layer computation
void denseLayer(float *inputs, float *weights, float *biases, float *outputs, int num_samples, int embedding_size) {
    clock_t start_time = clock(); // Start time measurement
    nvtxRangePush("denseLayer");
    #pragma acc parallel loop present(inputs[:num_samples * embedding_size], weights[:embedding_size], biases[:embedding_size], outputs[:num_samples])
    for (int i = 0; i < num_samples; i++) {
        outputs[i] = biases[0]; // Start with the bias
        for (int k = 0; k < embedding_size; k++) {
            outputs[i] += inputs[i * embedding_size + k] * weights[k]; // Only one output
        }
    }
    clock_t end_time = clock(); // End time measurement
    double execution_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    printf("Dense Layer Time: %.4f seconds\n", execution_time);

    nvtxRangePop();
}

// Apply sigmoid activation
void sigmoidActivation(float *outputs, int size) {
    nvtxRangePush("sigmoidActivation");
    clock_t start_time = clock(); // Start time measurement
    #pragma acc parallel loop present(outputs[:size])
    for (int i = 0; i < size; i++) {
        outputs[i] = 1.0f / (1.0f + expf(-outputs[i]));
    }
    clock_t end_time = clock(); // End time measurement
    double execution_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    printf("Sigmoid Activation Time: %.4f seconds\n", execution_time);
    nvtxRangePop();
}

// Evaluate model predictions
float evaluate(float *outputs, int *labels, int num_samples) {
    nvtxRangePush("evaluate");
    clock_t start_time = clock(); // Start time measurement
    int correct = 0;

    #pragma acc parallel loop reduction(+:correct) present(outputs[:num_samples], labels[:num_samples])
    for (int i = 0; i < num_samples; i++) {
        int predicted_label = outputs[i] > 0.6f ? 4 : 0; // If > 0.6, predict positive (4), else negative (0)
        //printf("Sample %d: True Label = %d, Predicted Label = %d\n", i, labels[i], predicted_label);
        if (predicted_label == labels[i]) {
            correct++;
        }
    }
    clock_t end_time = clock(); // End time measurement
    double execution_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    printf("Evaluation Time: %.4f seconds\n", execution_time);
    nvtxRangePop();
    return (float)correct / num_samples;
}

int main() {
    clock_t start_time = clock(); // Start time measurement

    printf("Starting program...\n");

    Post *trainSet = NULL, *testSet = NULL;
    int trainSize, testSize;
    int num_samples = loadAndSplitDataset("last_500000_rows.csv", &trainSet, &testSet, &trainSize, &testSize);

    if (trainSize == 0 || testSize == 0) {
        printf("Error: No samples found in dataset.\n");
        return 1;
    }

    printf("Loaded %d training samples and %d test samples.\n", trainSize, testSize);

    // Memory for the labels and token IDs for training
    int *trainLabels = (int *)safe_malloc(trainSize * sizeof(int), "trainLabels");
    for (int i = 0; i < trainSize; i++) {
        trainLabels[i] = trainSet[i].label;
    }

    float *trainTokenIds = (float *)safe_malloc(trainSize * MAX_TOKENS * sizeof(float), "trainTokenIds");
    float *trainWeights = (float *)safe_malloc(NUM_FEATURES * NUM_FEATURES * sizeof(float), "trainWeights");
    float *trainBiases = (float *)safe_malloc(NUM_FEATURES * sizeof(float), "trainBiases");
    float *trainOutputs = (float *)safe_malloc(trainSize * NUM_FEATURES * sizeof(float), "trainOutputs");

    srand(time(NULL));
    init_weights(trainWeights, NUM_FEATURES);

    for (int i = 0; i < NUM_FEATURES; i++) {
        trainBiases[i] = 0.0f;
    }

    #pragma acc enter data copyin(trainSet[:trainSize], trainLabels[:trainSize], trainTokenIds[:trainSize * MAX_TOKENS], trainWeights[:NUM_FEATURES * NUM_FEATURES], trainBiases[:NUM_FEATURES]) create(trainOutputs[:trainSize * NUM_FEATURES])

    // Tokenizing and embedding training dataset
    printf("Tokenizing and embedding training dataset...\n");
    tokenizeAndEmbed(trainSet, trainTokenIds, trainSize);

    // Train the model with the training set
    printf("Running dense layer...\n");
    denseLayer(trainTokenIds, trainWeights, trainBiases, trainOutputs, trainSize, NUM_FEATURES);

    // Apply sigmoid activation for training set
    printf("Applying sigmoid activation...\n");
    sigmoidActivation(trainOutputs, trainSize);

    // Evaluate on the test set (after training)
    printf("Evaluating model on test set...\n");
    float accuracy = evaluate(trainOutputs, trainLabels, trainSize); // Evaluating on training set here
    printf("Training set Accuracy: %.2f%%\n", accuracy * 100);

    // Now, evaluate on test set
    int *testLabels = (int *)safe_malloc(testSize * sizeof(int), "testLabels");
    for (int i = 0; i < testSize; i++) {
        testLabels[i] = testSet[i].label;
    }

    float *testTokenIds = (float *)safe_malloc(testSize * MAX_TOKENS * sizeof(float), "testTokenIds");
    float *testOutputs = (float *)safe_malloc(testSize * NUM_FEATURES * sizeof(float), "testOutputs");

    #pragma acc enter data copyin(testSet[:testSize], testLabels[:testSize], testTokenIds[:testSize * MAX_TOKENS]) create(testOutputs[:testSize * NUM_FEATURES])

    // Tokenizing and embedding test dataset
    printf("Tokenizing and embedding test dataset...\n");
    tokenizeAndEmbed(testSet, testTokenIds, testSize);

    // Use the trained model to make predictions on the test set
    printf("Running dense layer on test set...\n");
    denseLayer(testTokenIds, trainWeights, trainBiases, testOutputs, testSize, NUM_FEATURES);

    // Apply sigmoid activation for test set
    printf("Applying sigmoid activation on test set...\n");
    sigmoidActivation(testOutputs, testSize);

    // Evaluate the test set
    printf("Evaluating model on test set...\n");
    float testAccuracy = evaluate(testOutputs, testLabels, testSize);
    printf("Test set Accuracy: %.2f%%\n", testAccuracy * 100);



    #pragma acc exit data delete(trainSet[:trainSize], trainLabels[:trainSize], trainTokenIds[:trainSize * MAX_TOKENS], trainWeights[:NUM_FEATURES * NUM_FEATURES], trainBiases[:NUM_FEATURES], trainOutputs[:trainSize * NUM_FEATURES])
    #pragma acc exit data delete(testSet[:testSize], testLabels[:testSize], testTokenIds[:testSize * MAX_TOKENS], testOutputs[:testSize * NUM_FEATURES])
    
    clock_t end_time = clock(); // End time measurement
    double execution_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    printf("Total Execution Time: %.4f seconds\n", execution_time);

    // Free memory
    free(trainSet);
    free(testSet);
    free(trainLabels);
    free(testLabels);
    free(trainTokenIds);
    free(testTokenIds);
    free(trainWeights);
    free(trainBiases);
    free(trainOutputs);
    free(testOutputs);

    printf("Program completed.\n");
    return 0;
}

