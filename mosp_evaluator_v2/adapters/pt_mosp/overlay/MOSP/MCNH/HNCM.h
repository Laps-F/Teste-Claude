// Modification: Codex, 2026-10-29. Added explicit matrix orientation; supplied at no charge.
/************************************************************************************************************************************************
* MINIMAL COST NODE HEURISTIC																				*
/************************************************************************************************************************************************
* AUTHORS: MARCO ANTONIO MOREIRA DE CARVALHO (marco.opt@gmail.com) AND NEI YOSHIHIRO SOMA (soma@ita.br)											*
* AERONAUTICS TECHNOLOGICAL INSTITUTE - BRAZIL																									*
* DATE OF CREATION: 13/02/2012																													*
* LAST MODIFICATION: 13/02/2012																													*
/************************************************************************************************************************************************
* TERMS OF USE																																	*
*																																				*
* This code available for free public use.																										*
* The code in this archive is licensed gratis to all third parties under the terms of this paragraph.											*
* Copying and distribution of the files in this archive is unrestricted if and only if the files are not modified.								*
* Modification of the files is encouraged, but the distribution of modifications of the files in this archive is unrestricted only				*
* if you meet the following conditions: modified files must carry a prominent notice stating (i) the author and date,							*
* (ii) the new author and the date of release of the modification, (iii) that the work is licensed at no charge to all parties.					*
*																																				*
* If you use the code extensively in your research, you are requested to provide appropriate attribution and thanks to the author of the code.  *
/************************************************************************************************************************************************
* DISCLAIMER OF WARRANTY																														*
*																																				*
* This source code is provided "as is" and without warranties as to performance or merchantability.												*
* The authors and/or distributors of this source code may have made statements about this source code.											*
* Any such statements do not constitute warranties and shall not be relied on by the user in deciding whether to use this source code.			*
*																																				*
* This source code is provided without any express or implied warranties whatsoever. Because of the diversity of conditions and hardware under	*
*  which this source code may be used, no warranty of fitness for a particular purpose is offered.												*
* The user is advised to test the source code thoroughly before relying on it. The user must assume the entire risk of using the source code.	*
*																																				*
************************************************************************************************************************************************/

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include "LinkedList.h"				//includes the code of the Trotter-Johnson algorithm

typedef struct	{
                    int degree;		//stores the degree of the vertex
					int id;			//stores the index of the vertex
					int removed;	//indicates whether the vertex has been removed in MDGh procedure
					int open;	//indicates whether the vertex is a neighbour of a removed vertex in MDGh procedure
				}typeVertex;		//structure of a vertex

typedef struct	{
    int start;
    int end;	//indicates whether the vertex is a neighbour of a removed vertex in MDGh procedure
}typeArc;		//structure of an arc

typePattern *pattern;	//stores informations about the patterns, defined in LinkedList.h file
typeVertex *vertex;		//stores informations about the vertex
typeList *dominator;	//stores the dominated patterns;
typeArc *ARC;

int **matrix;			//stores the input data
int **adjacencyMatrix;	//stores the adjancency matrix of the MOSP graph, used as an upper diagonal matrix
int *stack;				//simulates the open stacks
int *demand;			//stores the demands for pieces (original)
int *demand2;			//stores the demands for pieces
int *demand3;			//stores the demands for pieces
int *solution;			//stores the pattern sequencing
int *solution2;			//stores the alternative pattern sequencing
int *spe;				//stores the pieces sequencing
int *degrees;			//stores the vertices degrees on the complementary MOSP graph
int *OPEN;
int *completion;
int nPatterns;			//number of patterns
int nPieces;			//number of pieces
int nPatternsOriginal;	//original number of patterns
int nOpenStacks;		//number of open stacks
int nArcs;

/*
Initializes the structures, vectors and matrices used
*/
void initialization()
{
	int i;

	matrix = (int**) calloc(nPatterns, sizeof(int*));

	if(!matrix)														//tests the memory allocation
	{
		printf("INSUFFICIENT MEMORY (A)\n");						//not enough memory to allocate the pointer
		system ("pause");
	}

	for(i=0; i<nPatterns; i++)
	{
		matrix[i] = (int*) calloc(nPieces, sizeof(int));

		if(!matrix[i])
		{
			printf("INSUFFICIENT MEMORY (B)\n");
			system ("pause");
		}
	}

	pattern = (typePattern*) calloc(nPatterns, sizeof(typePattern));

	if(!pattern)
	{
		printf("INSUFFICIENT MEMORY (C)\n");
		system ("pause");
	}

	stack = (int*) calloc(nPieces, sizeof(int));

	if(!stack)
	{
		printf("INSUFFICIENT MEMORY (D)\n");
		system ("pause");
	}

	demand = (int*) calloc(nPieces, sizeof(int));

	if(!demand)
	{
		printf("INSUFFICIENT MEMORY (E)\n");
		system ("pause");
	}

	solution = (int*) calloc(nPatterns, sizeof(int));

	if(!solution)
	{
		printf("INSUFFICIENT MEMORY (F)\n");
		system ("pause");
	}

    solution2 = (int*) calloc(nPatterns, sizeof(int));

	if(!solution2)
	{
		printf("INSUFFICIENT MEMORY (F)\n");
		system ("pause");
	}

	dominator = (typeList*) calloc(nPatterns, sizeof(typeList));

	if(!dominator)
	{
		printf("INSUFFICIENT MEMORY (G)\n");
		system ("pause");
	}

	vertex = (typeVertex*) calloc(nPieces, sizeof(typeVertex));

	if(!vertex)
	{
		printf("INSUFFICIENT MEMORY (H)\n");
		system ("pause");
	}

	adjacencyMatrix = (int**) calloc(nPieces, sizeof(int*));

	if(!adjacencyMatrix)
	{
		printf("INSUFFICIENT MEMORY (I)\n");
		system ("pause");
	}

	for(i=0; i<nPieces; i++)
	{
		adjacencyMatrix[i] = (int*) calloc(nPieces, sizeof(int));

		if(!adjacencyMatrix[i])
		{
			printf("INSUFFICIENT MEMORY (J)\n");
			system ("pause");
		}
	}

	spe = (int*) calloc(nPieces+1, sizeof(int));

	if(!spe)
	{
		printf("INSUFFICIENT MEMORY (K)\n");
		system ("pause");
	}

	degrees = (int*) calloc(nPieces, sizeof(int));

	if(!degrees)
	{
		printf("INSUFFICIENT MEMORY (L)\n");
		system ("pause");
	}

	demand2 = (int*) calloc(nPieces, sizeof(int));

	if(!demand2)
	{
		printf("INSUFFICIENT MEMORY (N)\n");
		system ("pause");
	}

    demand3 = (int*) calloc(nPieces, sizeof(int));

	if(!demand3)
	{
		printf("INSUFFICIENT MEMORY (O)\n");
		system ("pause");
	}

    ARC = (typeArc*) calloc(nPieces*nPieces, sizeof(typeArc));
    OPEN = (int*) calloc(nPieces, sizeof(int));

    if(!ARC || !OPEN)
    {
        printf("INSUFFICIENT MEMORY (P)\n");
        system ("pause");
    }

    completion = (int*) calloc(nPatternsOriginal, sizeof(int));

    if(!completion)
    {
        printf("INSUFFICIENT MEMORY (Q)\n");
        system ("pause");
    }


	for(i=0; i<nPatterns; i++)									//initializes some structures
	{
		pattern[i].removed = 0;
		pattern[i].size = 0;
		pattern[i].blocked = 0;
		emptyList(&dominator[i]);
	}

	for(i=0; i<nPieces; i++)
	{
		vertex[i].open = 0;
		vertex[i].degree = 0;
		vertex[i].id = i;
	}
}

/*
Reads the problem from a file specified by fileName
*/
void readProblem(char *fileName, bool patternsInRows)
{
	int i;
	int j;
	int k;
	
	FILE *fpIn = fopen(fileName, "r");						//input file

	//instancias do challenge precisam disso
	// char buffer[1024];
	// fgets(buffer, sizeof(buffer), fpIn); 
	
	fscanf(fpIn, "%d %d", &nPatterns, &nPieces);
	// fscanf(fpIn, "%d %d", &nPieces, &nPatterns);

	nPatternsOriginal = nPatterns;

	initialization();										//initializes all structures, vectors and matrices
	
    if(patternsInRows){
        for(i=0;i<nPatterns;++i)for(j=0;j<nPieces;++j){
            if(fscanf(fpIn,"%d",&matrix[i][j])!=1)throw std::runtime_error("MCNH: invalid matrix");
            if(matrix[i][j]==1)++pattern[i].size;
        }
    }else{
        for(j=0;j<nPieces;++j)for(i=0;i<nPatterns;++i){
            if(fscanf(fpIn,"%d",&matrix[i][j])!=1)throw std::runtime_error("MCNH: invalid matrix");
            if(matrix[i][j]==1)++pattern[i].size;
        }
    }

	fclose(fpIn);
}

/*
Builds de MOSP graph
*/

void buildMOSPGraph()
{
    int i, j, k;
    nArcs = 0;

	for(j=nPieces-1; j>=0; j--)
	{
		for(i=0; i<nPatternsOriginal; i++)
		{
			if((!pattern[i].removed)&&(matrix[i][j] == 1))
			{
  			    for(k=0; k<nPieces; k++)
				{
					if((j!=k)&&(matrix[i][k] == 1))			//updates the adjacency matrix
					{
						if (k<j)
						{
                            if (adjacencyMatrix[k][j]==0) {
                                nArcs++;
                            }
							adjacencyMatrix[k][j] = 1;
						}
						else
						{
							adjacencyMatrix[j][k] = 1;
                            if (adjacencyMatrix[j][k]==0) {
                                nArcs++;
                            }

						}
					}
				}

                 demand[j]++;
                 demand2[j]++;
            }
		}
	}

   //Determines the degree of each vertex on the MOSP graph
   for(i=0; i<nPieces; i++)
   {
       for(j=0; j<nPieces; j++)
       {
           if(i<j)
           {
               vertex[i].degree+=adjacencyMatrix[i][j];
           }
           else if(i>j)
           {
               vertex[i].degree+=adjacencyMatrix[j][i];
           }
       }
   }

   //stores the degree of the vertices
   for(i=0; i<nPieces; i++)
      degrees[i] = vertex[i].degree;
}

/*
Evaluates the current solution, simulating the opened and closed stacks
*/
int evaluation(int limit, int *solution)
{
	int i;
	int j;
	int index;
	int openStacks;
	int closedStacks;
	int maxOpenStacks;

	openStacks = 0;
	closedStacks = 0;
	maxOpenStacks= -1;

	for(i=0; i<nPieces; i++)
	{
		demand3[i]=demand2[i];
		stack[i] = 0;
	}

	for(i=0; i<limit; i++)
	{
		index = solution[i];				//simulates the sequencing of the current pattern

		for(j=0; j<nPieces; j++)
		{
			if(matrix[index][j] > 0)
			{
				demand3[j]--;				//decreases the demand for each piece of the pattern

				if (demand3[j] == 0)
					closedStacks++;			//simulates closed stacks

				if(stack[j] == 0)
				{
					openStacks++;			//simulates new open stacks
					stack[j] = 1;
				}
			}
		}

		if (openStacks > maxOpenStacks)
			maxOpenStacks = openStacks;		//stores the maximum number of open stacks

		openStacks -= closedStacks;
		closedStacks = 0;
	}

	return maxOpenStacks;					//returns the maximum number of open stacks
}

/*
 Gives the pattern sequencing correspondent to the piece sequencing, as in Becceneri (2004)
 */
void patternSequencing()
{
	int i;
	int j;
	int index = 0;


    for(i=0; i<nPatternsOriginal; i++)
    {
        for(j = 0; j<nPieces; j++)
           if((!pattern[i].removed)&&(matrix[i][j] == 1))
              completion[i]++;
    }

	for(i = 0; i<nPieces; i++)//trocar do início para o final
    {
        for(j=0; j<nPatternsOriginal; j++)
        {
			if((!pattern[j].blocked)&&(!pattern[j].removed))
			{
				if(matrix[j][spe[i]] == 1)
				{
                    completion[j]--;

                    if(completion[j] == 0)
                    {
                       pattern[j].blocked = 1;
					   solution[index++] = j;	//inserts it in the pattern sequence from the left to the right
                    }
				}
			}
		}
	}

    for(i=0; i<nPatternsOriginal; i++)
        pattern[i].blocked=0;
}

int findMinimumDegree()
{
	int index  = -1;
	int i;
	int smaller = nPieces*nPieces+1;

	for(i=0; i<nPieces; i++)
	{
		if((vertex[i].degree < smaller)&&(vertex[i].degree>0)&&(!vertex[i].removed))
		{
			smaller = vertex[i].degree;													//selects the vertex of smaller degree not sequenced yet
			index = i;																	//stores its index
		}
	}

	return index;
}

void HNCM()
{
    int omega, s=0, i, j, n1, n2, pairwise, openIndex = 0, arcIndex = 0;

    while(s!=nArcs)
    {
        pairwise=10000000;
        omega = findMinimumDegree();//gets the minimal degree vertex index
        for(i=0; i<nPieces; i++)
        {
           if(vertex[i].degree == vertex[omega].degree)
           {
              for(j=0; j<nPieces; j++)
              {
                 if(((adjacencyMatrix[i][j]==1)||(adjacencyMatrix[j][i]==1))&&(vertex[i].degree+vertex[j].degree)<pairwise)
                 {
                     pairwise = vertex[i].degree+vertex[j].degree;
                     n1 = i;
                     n2 = j;
                 }
              }
           }
        }

        adjacencyMatrix[n1][n2]=0;
        adjacencyMatrix[n2][n1]=0;
        if(!vertex[n1].open)
        {
           OPEN[openIndex++] = n1;
           vertex[n1].open = 1;
        }

        if(!vertex[n2].open)
        {
            OPEN[openIndex++] = n2;
            vertex[n2].open = 1;
        }

        ARC[arcIndex].start = n1;
        ARC[arcIndex++].end = n2;

        s++;

        vertex[n1].degree--;

        if(vertex[n1].degree == 0)
            vertex[n1].removed = 1;

        vertex[n2].degree--;

        if(vertex[n2].degree == 0)
            vertex[n2].removed = 1;

        for(i=0; i<openIndex; i++)
        {
           for(j=0; j<openIndex; j++)
           {
              n1 = OPEN[i];
              n2 = OPEN[j];
              if(adjacencyMatrix[n1][n2]==1 || adjacencyMatrix[n2][n1]==1)
              {
                  adjacencyMatrix[n1][n2]=0;
                  adjacencyMatrix[n2][n1]=0;
                  ARC[arcIndex].start = n1;
                  ARC[arcIndex++].end = n2;

                  s++;

                  vertex[n1].degree--;
                  if(vertex[n1].degree == 0)
                      vertex[n1].removed = 1;

                  vertex[n2].degree--;
                  if(vertex[n2].degree == 0)
                      vertex[n2].removed = 1;
              }
           }
        }
    }
	
    // for(i=0; i<nPieces; i++)
    //     spe[i] = OPEN[i];

	int speIndex = 0;

    // 1. Copia todas as peças que foram encontradas pela heurística
    //    (openIndex foi sendo incrementado quando adicionamos em OPEN)
    for (i = 0; i < openIndex; i++) {
        spe[speIndex++] = OPEN[i];
    }

    // 2. Adiciona todas as peças isoladas (que nunca foram marcadas como .open)
    for (i = 0; i < nPieces; i++) {
        if (!vertex[i].open) { // Se a flag 'open' for 0, a peça foi isolada
            spe[speIndex++] = i;
        }
    }

    // Opcional: Verificação
    if (speIndex != nPieces) {
         fprintf(stderr, "Erro: A sequencia de pecas esta incompleta. Esperado %d, obtido %d\n", nPieces, speIndex);
    }
}

/*
 Pre-processing procedure, detects and removes dominated patterns
 */
void dominationPreProcessing()
{
	int i;
	int j;
	int k;
	int flag = 1;
	int index;
	int index2;
	int counter = 0;

	for(i=0; i<nPatterns; i++)
	{
		if(!pattern[i].removed)
		{
			for(j=i+1; j<nPatterns; j++)
			{
				if(!pattern[j].removed)
				{
					if(pattern[i].size < pattern[j].size)							//if pattern i is smaller than pattern j
					{
						index = i;
						index2 = j;
						for(k=0; k<nPieces; k++)
						{
							if ((matrix[i][k] == 1) && (matrix[j][k] == 0))			//and pattern i has a piece that pattern j doesn't
							{
								flag = 0;											//then i is not dominated by j
								break;
							}
						}
					}
					else	if(pattern[i].size == pattern[j].size)					//if patterns i and j have the same size
                    {
                        index = j;
                        index2 = i;
                        for(k=0; k<nPieces; k++)
                        {
                            if (matrix[i][k] != matrix[j][k])				//and they differ in any position
                            {
                                flag = 0;									//then no one is dominated
                                break;
                            }
                        }
                    }
                    else	if((pattern[i].size > pattern[j].size))			//if pattern i is larger than pattern j
                    {
                        index = j;
                        index2 = i;
                        for(k=0; k<nPieces; k++)
                        {
                            if ((matrix[i][k] == 0) && (matrix[j][k] == 1)) //and j has a piece that i doesn't
                            {
                                flag = 0;									//then j is not dominated by i
                                break;
                            }
                        }
                    }

					if (flag == 1)													//if the a dominance condition is met
					{
						pattern[index].removed = 1;									//the pattern is removed
						counter++;

						add(&dominator[index2], index);

						if(count(&dominator[index]) > 0)							//if the dominated pattern dominates other patterns
							transferDominated(&dominator[index2], &dominator[index]);		//the list is transferred
						if(index == i)
							break;
					}
				}

				flag = 1;
			}
		}
	}

	nPatternsOriginal = nPatterns;													//stores the original number of patterns
	nPatterns -= counter;															//updates the number of patterns
}

/*
Terminates all lists, vectors and matrices. Frees all pointers
*/
void termination()
{
	int i;

	for(i=0; i<nPatternsOriginal; i++)
	{
		finalize(&dominator[i]);
		free(matrix[i]);
	}

	for(i=0; i<nPieces; i++)
		free(adjacencyMatrix[i]);

	free(dominator);
	free(matrix);
	free(adjacencyMatrix);
	free(pattern);
	free(stack);
	free(demand);
	free(demand2);
	free(demand3);
	free(solution);
    free(solution2);
	free(vertex);
	free(spe);
	free(degrees);
    free(ARC);
    free(OPEN);
    free(completion);
}

char *getBaseName(char *path) {
    char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

/*
Main procedure of the constructive heuristic
Parameters:
HNCMResult	stores the solution value obtained by the heuristic
HNCMTime		stores the exeution time of the heuristic
*/
void mainMethod(int *HNCMResult, double *HNCMTime, char *inputFileName, int **externalSolution, int *externalSolutionSize, int pieces, bool patternsInRows = false)
{
	int i;
	int j;
	int start = 0;
	int end;
	int result;
	int result2;
    int resultb;

	double startHNCM;
	double endHNCM;

    char outputFileName[256];
	char *baseName = getBaseName(inputFileName);

	// FILE *fpSolution;
	// sprintf(outputFileName, "Solution_%s", baseName);
	// fpSolution = fopen(outputFileName, "w");				//file that contains the information about the solution of a problem instance

	readProblem(inputFileName,patternsInRows);								//reads the problem data
	startHNCM = clock();										//time taking

    // dominationPreProcessing();								//preprocessing procedure, removes dominated patterns
    buildMOSPGraph();

	HNCM();

    patternSequencing();
    result = evaluation(nPatterns, solution);
	endHNCM = clock();										//time taking

	*HNCMResult = result;									//stores the solution value
	*HNCMTime =  (endHNCM - startHNCM)/CLOCKS_PER_SEC;			//stores the execution time

	// fprintf(fpSolution, "Maximum Open Stacks: %d\n", result);
	
	// fprintf(fpSolution, "Number of Pieces: %d, Number of Patterns: %d, Patterns eliminated in the Pre-processing: %d\n", nPieces, nPatternsOriginal, nPatternsOriginal-nPatterns);
	// fprintf(fpSolution, "Patterns Sequence:\n");

	for(i=0; i<nPatterns; i++)
	{
		// fprintf(fpSolution, "%d \n", solution[i]+1);			//prints the indexes (from 0 to nPatterns-1) of the patterns, one per line

		if(count(&dominator[solution[i]]) > 0)				//prints the dominated patterns right after the patterns they are dominated by
		{
			// print(&dominator[solution[i]], fpSolution);		//after the index of the pattern the word [dominated] is written to indicate it
		}
	}

	// fprintf(fpSolution, "\nSolution Matrix:\n");

	for(i=0; i<nPatterns; i++)
	{
		for(j=0; j<nPieces; j++)
		{
			// fprintf(fpSolution, "%d ", matrix[solution[i]][j]);
		}

		// fprintf(fpSolution, "\n");

		if(count(&dominator[solution[i]]) > 0)									//prints the dominated patterns right after the patterns they are dominated by
		{
			// print2(&dominator[solution[i]], fpSolution, nPieces, matrix);		//after the index of the pattern the word [dominated] is written to indicate it
		}
	}

	// fclose(fpSolution);

	if(pieces){
		*externalSolution = new int[nPieces];
		*externalSolutionSize = nPieces;


		for(int i = 0; i < nPieces; i++) {
			(*externalSolution)[i] = spe[i];
		}
	} else {
		*externalSolution = new int[nPatterns];
		*externalSolutionSize = nPatterns;


		for(int i = 0; i < nPatterns; i++) {
			(*externalSolution)[i] = solution[i];
		}
	}

	termination();
}