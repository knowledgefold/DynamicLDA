/*
 * TopicChains.cpp
 *
 *  Created on: Apr 22, 2014
 *      Author: vspathak
 */

#include <math.h>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <omp.h>
#include <iostream>
#include <fstream>
#include <string.h>
#include <time.h>
#include <random>
#include <cstring>
#include <map>
#include <limits>
#include <boost/config.hpp>
#include <algorithm>
#include <utility>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/connected_components.hpp>

using namespace std;
using namespace boost;

typedef adjacency_list <vecS, vecS, undirectedS> Graph;


// Initialize number of documents, topics and words in vocabulary
unsigned int W, D, K;

double diffclock(clock_t clock1, clock_t clock2) {
    double diffticks = clock1 - clock2;
    double diffms = (diffticks * 1000) / CLOCKS_PER_SEC;
    return diffms;
}

double KLDivergence(double*** Pi, int t, int k, double* M) {
    double result = 0.0;
    for (unsigned int w = 0; w < W; ++w) {
        result += log(Pi[t][w][k] / M[w]) * Pi[t][w][k];
    }
    return result;
}

double JSsimilarity(double*** Pi, int t1, int k1, int t2, int k2) {
    double result = 0.0;
    double* M = new double[W];
    for (unsigned int w = 0; w < W; ++w) {
        M[w] = (Pi[t1][w][k1] + Pi[t2][w][k2]) / 2;
    }
    result = KLDivergence(Pi, t1, k1, M) + KLDivergence(Pi, t2, k2, M);
    result = result / 2;
    return result;
}

void generateTopicLinks(Graph &G, double*** Pi, int timeSlice, int topic, int numTopics, int windowSize, double threshold) {
	for (int w = 0; w < windowSize; w++) {
		int numLinks = 0;
		for (int k = 0; k < numTopics; k++) {
			if ((timeSlice - 1 - w >= 0)
					&& JSsimilarity(Pi, timeSlice, topic, timeSlice - 1 - w, k) > threshold) {
				//add edge to graph structure here
				int e1 = (timeSlice * numTopics) + topic;
				int e2 = ((timeSlice - 1 - w) * numTopics) + k;

				cout<<"Adding edge "<<e1<<", "<<e2<<endl;
				add_edge(e1, e2, G);
				numLinks++;
			}
		}
		if (numLinks > 0) {
			break;
		}
	}
}

void generateAllLinks(Graph &G, double*** Pi, int numTimeSlices, int numTopics, int windowSize, double threshold) {
    for (int t = 0; t < numTimeSlices; t++) {
        for (int k = 0; k < numTopics; k++) {
            generateTopicLinks(G, Pi, t, k, numTopics, windowSize, threshold);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printf("Usage: ./fastLDA inputfile num_iterations num_topics\n");
        return 1;
    }

    // Initlialize expected topic counts per document
    double **nTheta;
    // Dynamically
    double **nPi;
    double *N_z;
    // Initialize estimates from each minibatch
    // Initialize step sizes
    double rhoTheta = 0;
    double rhoPhi = 0;
    double ***Pi;
    double **theta;
    double **perplexities;
    // Initlalize dirichlet prior parameters
    double alpha, eta;
    double M; // Number of documents in each minibatch
    int Cj = 0;
    unsigned int i, j, k, w, MAXITER;
    int batch_idx = 0;
    int C = 0;
    int iter = 0;
    int NNZ;

    int windowSize = 0;
    double similarityThreshold = 0;

    ofstream pfile;
    pfile.open("perplexity.txt");

    M = 100; //343 works for KOS and only for KOS
    eta = 0.01; // was 0.01
    alpha = 0.1;

    ifstream seqfile;
    seqfile.open("Data/seqfile.txt");
    string newline = "";
    vector<int>* months = new vector<int>();
    vector<int>* numOfDocs = new vector<int>();
        vector<int>* monthFirstIdx = new vector<int>();
        vector<int>* monthLastIdx = new vector<int>();
        int curIdx = 0;

    while (seqfile >> newline) {
        const char * ptr = strchr(newline.c_str(), ':');
        int count = atoi(ptr + 1);
        ptr = "\0";
        int yearMonth = atoi(newline.c_str());
        months->push_back(yearMonth);
        numOfDocs->push_back(count);
                monthFirstIdx->push_back(curIdx);
                monthLastIdx->push_back(curIdx+count);
                curIdx += count;
    }
    seqfile.close();

    //if user also specified a minibatch size
    if (argc > 4) {
        M = atof(argv[4]);
        windowSize = atoi(argv[6]);
        similarityThreshold = atof(argv[7]);
    }

    MAXITER = atoi(argv[2]);
    K = atoi(argv[3]);

    printf("Input file: %s\n", argv[1]);
    printf("Number of iterations: %d\n", MAXITER);
    printf("Number of topics: %d\n", K);
    printf("Minibatch size: %f\n", M);
    printf("alpha:  %f\n", alpha);
    printf("eta:  %f\n", eta);

    // Read the file and store it in DATA
    FILE* fptr;
    unsigned int docnum, wnum;
    unsigned char countnum;

    fptr = fopen(argv[1], "rt");

    fscanf(fptr, "%d\n", &D);
    fscanf(fptr, "%d\n", &W);
    fscanf(fptr, "%d\n", &NNZ);

    printf("Number of documents: %d\n", D);
    printf("Vocabulary size: %d\n", W);

    // Dynamically allocate phi
    Pi = new double**[months->size()];
    for (unsigned int m = 0; m < months->size(); ++m) {
        Pi[m] = new double*[W];
        for (unsigned int word = 0; word < W; word++) {
            Pi[m][word] = new double[K];
        }
    }
//#pragma omp parallel for


    printf("allocated phi\n");

    // Dynamically allocate theta

    theta = new double*[D];
//#pragma omp parallel for
    for (i = 0; i < D; i++) {
        theta[i] = new double[K];
    }

    printf("allocated theta\n");

    vector<vector<int> > corpus;
    vector<int> corpus_size(D, 0);
    corpus.resize(D);
    vector<vector<int> > corpus_expanded;
    corpus_expanded.resize(D);

    while (!feof(fptr)) {
        fscanf(fptr, "%d %d %hhu\n", &docnum, &wnum, &countnum);

        corpus[docnum - 1].push_back(wnum - 1);
        corpus[docnum - 1].push_back(countnum);

        corpus_size[docnum - 1] += countnum;

        for (i = 0; i < countnum; i++) {
            corpus_expanded[docnum - 1].push_back(wnum - 1);
        }
    }
    fclose(fptr);


    // Initialize phi_est and all other arrays
    nPi = new double*[W];

    for (i = 0; i < W; i++) {
        nPi[i] = new double[K];
    }

    // Initialize n_z and n_z_est and other arrays
    N_z = new double[K];
    for (k = 0; k < K; k++) {
        N_z[k] = 0;
    }

    nTheta = new double*[D];
    for (i = 0; i < D; i++) {
        nTheta[i] = new double[K];
    }

    for (i = 0; i < D; i++) {
        for (k = 0; k < K; k++) {
            nTheta[i][k] = rand() % 10;
        }
    }

    perplexities = new double*[months->size()];
    for (i = 0; i < months->size(); i++) {
        perplexities[i] = new double[K];
        for (unsigned int a = 0; a < K; ++a) {
            perplexities[i][a] = 0;
        }
    }

    int*** topwords;
    topwords = new int**[months->size()];

    //Generate Numbers according to Gaussian Distribution

    for (int timeSlice = 0; timeSlice < 10; timeSlice++) {
        cout << (*months)[timeSlice] << " " << (*numOfDocs)[timeSlice] << endl;

        //if parallelizing this, make sure to avoid race condition (most likely use reduction)
        for (k = 0; k < K; k++) {
            for (w = 0; w < W; w++) {
                N_z[k] += nPi[w][k];
            }
        }

        // Find the total number of word in the document
        int monthFirstDoc = monthFirstIdx->at(timeSlice);
        int monthLastDoc = monthLastIdx->at(timeSlice);

        int monthD = monthLastDoc - monthFirstDoc;

        C = 0;

        for (j = monthFirstDoc; j < monthLastDoc; j++) {
            C += corpus_size[j];
        }

        printf("Number of words in corpus: %d\n", C);

        int firstdoc = 0;
        int lastdoc = 0;
        int DM = monthD / M;

        for (iter = 0; iter < (int)MAXITER; iter++) {
            // Decide rho_phi and rho_theta
            rhoPhi = 10 / pow((1000 + iter), 0.9);
            rhoTheta = 1 / pow((10 + iter), 0.9);

#pragma omp parallel private(batch_idx,j,k,i,w,firstdoc,lastdoc)
            {
                double *gamma = new double[K];
                double *nzHat = new double[K];
                double **nPhiHat = new double *[W];
                for (k = 0; k < K; k++) {
                    gamma[k] = 0;
                    nzHat[k] = 0;
                }
                for (i = 0; i < W; i++) {
                    nPhiHat[i] = new double[K];
                    for (k = 0; k < K; k++) {
                        nPhiHat[i][k] = 0;
                    }
                }

#pragma omp for
                for (batch_idx = 0; batch_idx < DM+1; batch_idx++) {

                    // Decide the document indices which go in each minibatch
                    firstdoc = monthFirstDoc + (batch_idx * M);
                    lastdoc = monthFirstDoc + ((batch_idx + 1) * M);

                    if (batch_idx == DM) {
                        lastdoc = monthLastDoc;
                    }
                    for (j = (unsigned)firstdoc; j < (unsigned)lastdoc; j++) {

                        // First perform the burn-in passes
                        // Iteration of burn in passes

                        // Store size of corpus in Cj
                        Cj = corpus_size[j];

                        for (i = 0; i < (corpus[j].size() / 2); i++) {// indexing is very different here!

                            int w_aj = corpus[j][2 * i];
                            int m_aj = corpus[j][(2 * i) + 1];
                            // Update gamma_ij and N_theta
                            double normSum = 0;

                            for (k = 0; k < K; k++) {
                                gamma[k] = (nPi[w_aj][k] + eta) * (nTheta[j][k] + alpha) / (N_z[k] + (eta * W));
                                normSum += gamma[k];
                            }

                            for (k = 0; k < K; k++) {
                                gamma[k] = gamma[k] / normSum;
                            }

                            for (k = 0; k < K; k++) {

                                nTheta[j][k] = (pow((1 - rhoTheta), m_aj) * nTheta[j][k])
                                        + ((1 - pow((1 - rhoTheta), m_aj)) * Cj * gamma[k]);
                            }

                        }

                        // Iteration of the main loop
                        for (i = 0; i < (corpus[j].size() / 2); i++) { // indexing is very different here!

                            int w_aj = corpus[j][2 * i];
                            int m_aj = corpus[j][(2 * i) + 1];
                            double normSum = 0;
                            for (k = 0; k < K; k++) {
                                gamma[k] = (nPi[w_aj][k] + eta) * (nTheta[j][k] + alpha) / (N_z[k] + (eta * W));
                                normSum += gamma[k];
                            }

                            for (k = 0; k < K; k++) {
                                gamma[k] = gamma[k] / normSum;
                            }

                            // Update N_theta estimates
                            for (k = 0; k < K; k++) {
                                nTheta[j][k] = (pow((1 - rhoTheta), m_aj) * nTheta[j][k])
                                        + ((1 - pow((1 - rhoTheta), m_aj)) * Cj * gamma[k]);

                                nPhiHat[w_aj][k] = nPhiHat[w_aj][k] + (C * gamma[k] / M);

                                nzHat[k] = nzHat[k] + (C * gamma[k] / M);
                            }
                        }

                    } // End of j

                    // Update the estimates matrix
                    for (k = 0; k < K; k++) {
                        for (w = 0; w < W; w++) {
                            nPi[w][k] = (1 - rhoPhi) * nPi[w][k] + rhoPhi * nPhiHat[w][k];
                        }
#pragma omp atomic
                        N_z[k] *= (1 - rhoPhi);
#pragma omp atomic
                        N_z[k] += rhoPhi * nzHat[k];
                    }

                } // End of batch_idx

                // Compute phi
#pragma omp for
                for (k = 0; k < K; k++) {
                    double normSum = 0;
                    for (w = 0; w < W; w++) {
                        nPi[w][k] += eta;
                        normSum += nPi[w][k];
                    }
//                    cout << normSum << endl;
                    for (w = 0; w < W; w++) {
                        Pi[timeSlice][w][k] = (double) nPi[w][k] / normSum;
                    }
                }

                // Compute theta
#pragma omp for
                for (i = monthFirstDoc; i < monthLastDoc; i++) {
                    double normSum = 0;
                    for (k = 0; k < K; k++) {
                        nTheta[i][k] += alpha;
                        normSum += nTheta[i][k];
                    }
                    for (k = 0; k < K; k++) {
                        theta[i][k] = (double) nTheta[i][k] / normSum;
                    }
                }

                delete[] gamma;
                delete[] nzHat;

                for (i = 0; i < W; i++) {
                    delete[] nPhiHat[i];
                }

                delete[] nPhiHat;

            }

        } // End of iter

        //write doctopics file
        /*ofstream dtfile;
        dtfile.open("output/doctopic_" + to_string(months->at(timeSlice)) + ".txt");
        for (i = monthFirstDoc; i < monthLastDoc; i++) {
            for (k = 0; k < K; k++) {
                dtfile << theta[i][k] << ",";
            }
            dtfile << endl;
        }
        dtfile.close();*/

        //compute the top 100 words for each topic

//        double** maxval;
//        topwords[timeSlice] = new int*[K];
//        maxval = new double*[K];
//        for (k = 0; k < K; k++) {
//            topwords[timeSlice][k] = new int[100];
//            maxval[k] = new double[100];
//        }
//        for (k = 0; k < K; k++) {
//            double oldMax = std::numeric_limits<double>::max();
//            for (i = 0; i < 100; i++) {
//                double max = -1;
//                int max_idx = -1;
//                for (w = 0; w < W; w++) {
//                    if (oldMax > Pi[timeSlice][w][k] && Pi[timeSlice][w][k] > max) {
//                        max = Pi[timeSlice][w][k];
//                        max_idx = w;
//                    }
//                }
//                oldMax = Pi[timeSlice][max_idx][k];
//                topwords[timeSlice][k][i] = max_idx;
//                maxval[k][i] = max;
//            }
//        }
    }//All timeSlices finished

    // MAKE CHAINS
    Graph G;
    //K is unsigned -- is this a problem?
//    generateAllLinks(G, Pi, months->size(), K, windowSize, similarityThreshold);
    generateAllLinks(G, Pi, 10, K, windowSize, similarityThreshold);

    vector<int> component(num_vertices(G));
    int num = connected_components(G, &component[0]);

    vector<int>::size_type p;
    cout << "Total number of components: " << num << endl;
    for (p = 0; p != component.size(); ++p)
   		cout << "Vertex " << p <<" is in component " << component[p] << endl;

//    string *dict;
//    dict = new string[W];
////    char word;
//    //retrieve the words from the file
//    w = 0;
//    string line;
//    ifstream vocabFile(argv[5]);
//    if (vocabFile.is_open()) {
//        while (getline(vocabFile, line)) {
//            dict[w] = line;
//            w++;
//        }
//        vocabFile.close();
//    }

//    write topics file
//    for (int timeSlice = 0; timeSlice < (int) months->size(); timeSlice++) {
//    for (int timeSlice = 0; timeSlice < 10; timeSlice++) {
//        ofstream tfile;
//        tfile.open("output/topics_" + to_string(months->at(timeSlice)) + ".txt");
//        for (k = 0; k < K; k++) {
//            for (w = 0; w < 100; w++) {
//                tfile << topwords[timeSlice][k][w];// << ":" << maxval[k][w] << ",";
//
//            }
//            tfile << endl;
//
//            for (w = 0; w < 100; w++) {
//                tfile << dict[topwords[timeSlice][k][w]] << ",";
//            }
//
//            tfile << endl;
//        }
//        tfile.close();
//    }

    return (0);

} // End of main
