/* ---------------------------------------------------------------------------
   Simple dynamic programming solution to challenge problem

   copyright Maria Garcia de la Banda and Peter Stuckey, May 2005

   Input format
-----------------------------
   [OPT] Name of benchmark on one line
   number-of-customers  number-of-products
   
   player 1: 0-1 (number-of-products) 
   player 2: 0-1 (number-of-products) 
   ...
   player n: 0-1 (number-of-products) 
 
-------------------------------
Example the challenge-small problem
-------------------------------

5 9

1 1 0 1 0 1 1 0 1  
1 1 0 1 1 1 0 1 0  
1 1 0 0 0 0 1 1 0  
1 0 0 0 1 1 0 0 1  
0 0 1 0 1 1 1 1 0  

-------------------------------

Returns the order where the minimum number
of customers need to be having their orders stacked
at the same time.

Example the challenge-small problem
-------------------------------
lowerbound = 4, upperbound = 5
mincost = 5, ccc = 171,loopcount=926

FLAGS:
     -d: make definite choices, that must be optimal
     -i n: iteration limit, most calls to mincost
     -t: show statistics of operations
     -v: Verbose output: show the actual solution

===============================================================================
   FOR the CHALLENGE PROBLEMS IT WAS COMPILED AS

       cc -DLARGE -DLOWMEM -O3 -lm

   AND RUN WITH FLAGS

       -b -o -i 33554432
   
===============================================================================
---------------------------------------------------------------------------- */
#define MYDEBUG 0

#include "solver.h"

struct rusage starttime,optimaltime,totaltime,runtime,endtime,opttime;

long long mystart;

char name[100];

int playbit[ARRAYPRODUCTS][MAXCUSTOMERS];
int prod_order[ARRAYPRODUCTS];
int cust_order[MAXCUSTOMERS];
int remove_order[MAXCUSTOMERS];

SET plays[ARRAYPRODUCTS];
SET cross[MAXCUSTOMERS];
SET ALLCUSTOMERS;

SET orig_cross[MAXCUSTOMERS];

int global_upperbound;
int global_lowerbound;
int given_upper = MAXCUSTOMERS;
int given_lower = 1;

int upperbound;
bool found_solution;
int num_relax = 0;
int close_open = 0;
int close_open_level = -1;

int CUSTOMERS;
int PRODUCTS;

bool benchmark = 0;
bool verbose = 0;
bool showstats = 1;
bool definite_move = 0;
bool better_move = 1;
bool old_move = 1;
bool use_hash = 1;
bool sort_moves = 1;
bool probe = 0;
bool relaxation = 1;

int relaxes = 0;
int relax_suc = 0;
int wasted = 0;

class Node {
public:
	SET custs_open;
	SET custs;
	SET seconded;

	int level;
	int now_open;
	int free_stacks;
  int num_moves;
	SET played;
	bool dm;
	bool freeze;

	int played_queue[MAXCUSTOMERS];
	int num_played;
	int pqhead;

	SET allopenset[MAXCUSTOMERS];
	SET openset[MAXCUSTOMERS];
	SET closeset[MAXCUSTOMERS];
	int open[MAXCUSTOMERS];
	int close[MAXCUSTOMERS];
	int moves[MAXCUSTOMERS];
	double scores[MAXCUSTOMERS];

	SET mergeset;
	int num_merged;
	int removed_nodes[MAXCUSTOMERS];
	int merged_nodes[MAXCUSTOMERS];
	SET old_cross[MAXCUSTOMERS];

	void search();
	inline void initnode();
	inline void relax();
	inline void findunmerge();
	inline void mergenodes(int a, int b);
	inline void unmergenodes();
	inline void mergemoves();
	inline void finddefinitemove();
	inline void prunenotopen();
	inline void findoldmoves();
	inline void rankmoves();
	inline void sortmoves();
	inline void pruneworsemoves();
	inline void playmove();
	inline void finish();
	inline void getsolution();
	inline void extendsolution();
	inline void dumpnode();
	inline bool checkcompress();
};

Node nodes[MAXCUSTOMERS];

void Node::initnode() {
	level = this-nodes;
	num_moves = 0;
	num_played = 0;
	played = emptyset;
	pqhead = 0;
	dm = false;
}

// look for solution that satisfies upperbound, using only customer graph
void Node::search() {
	if (probe && ccc[process_id] >= 100000) return;

	initnode();

	for (int i = 0; i < CUSTOMERS; i++) {
		if (!in(i,custs)) continue;
		if (cross[i].subset(custs_open)) custs.removebit(i);
	}

	if (custs == emptyset) {getsolution(); return;}
	if (use_hash && hash(custs_open & custs,custs)) {reuse[process_id]++; return;}

	now_open = (custs_open & custs).nbitsp();
	free_stacks = upperbound - now_open;		
	assert(now_open < upperbound);

	for (int i = 0; i < CUSTOMERS; i++) {
		if (!in(i,custs)) continue;
		loopcount[process_id]++;
		openset[i] = cross[i] - custs_open;
		open[i] = openset[i].nbitsp();
		if (open[i] <= free_stacks)	moves[num_moves++] = i;
	}

	if (MYDEBUG) dumpnode();

	mergemoves();

	if (definite_move) finddefinitemove();
	if (close_open || old_move) findoldmoves();
	if (close_open || better_move) pruneworsemoves();
	if (sort_moves) sortmoves();
	if (probe || close_open) prunenotopen();

	if (MYDEBUG) dumpnode();

	while (num_moves) {
		playmove();
		if (found_solution) break;
		if (close_open || better_move) pruneworsemoves();
	}

	finish();

}

inline void Node::mergemoves() {
	int t = 0;
	for (int i = 0; i < num_moves; i++) {
		int best = moves[i];
		int best_i = i;
		for (int j = i+1; j < num_moves; j++) {
			if (open[moves[j]] < open[best] || (open[moves[j]] == open[best] && openset[moves[j]] < openset[best])) {// ||	(openset[moves[j]] == openset[best] && moves[j] < best)) {
				best = moves[j];
				best_i = j;
			}
		}

		moves[best_i] = moves[i];

		int cur = t ? moves[t-1] : -1;
		if (cur == -1 || openset[best] != openset[cur]) {
			cur = best;
			moves[t++] = cur;
			allopenset[cur] = emptyset;
			closeset[cur] = emptyset;
			close[cur] = 0;
		}

		allopenset[cur] = allopenset[cur] + cross[best];
		closeset[cur].addbit(best);
		close[cur]++;

	}
	num_moves = t;
}

inline void Node::relax() {
	mergeset = emptyset;
	num_merged = 0;

	while (custs.nbitsp() > upperbound) {
		double w[CUSTOMERS];
		int best_j[CUSTOMERS];

		for (int i = 0; i < CUSTOMERS; i++) {
			if (!in(i,custs)) continue;
			w[i] = 1.0/(cross[i]-custs_open).nbitsp();
		}

		double safest = 1e100;
		int best_i = -1;

		for (int i = 0; i < CUSTOMERS; i++) {
			if (in(i,custs_open)) continue;
			double total_danger = 0;
			double total_weight = 0;
			double most_dangerous = -1;
			best_j[i] = -1;
			for (int j = 0; j < CUSTOMERS; j++) {
				if (!in(j,custs) || !in(j,cross[i]) || i == j) continue;
				double danger = w[j] * (cross[i] - custs_open - cross[j]).nbitsp();
				total_danger += danger;
				total_weight += w[j];
				if (danger > most_dangerous) {
					most_dangerous = danger;
					best_j[i] = j;
				}
			}
			if (best_j[i] == -1) continue;
			total_danger -= most_dangerous;
			double safety = total_danger / total_weight;
			if (safety < safest) {
				safest = safety;
				best_i = i;
			}
		}
		if (best_i == -1) return;

		mergenodes(best_i,best_j[best_i]);
	}
}



inline void Node::findunmerge() {
	int old_removed_nodes[num_merged];
	int old_merged_nodes[num_merged];
	int old_cust_order[CUSTOMERS];
	int old_num_merged = num_merged;

	if (verbose) printf("Find unmerge %d\n", num_merged-1);

	for (int i = 0; i < CUSTOMERS; i++) old_cust_order[i] = cust_order[i];
	for (int i = 0; i < num_merged; i++) {
		old_removed_nodes[i] = removed_nodes[i];
		old_merged_nodes[i] = merged_nodes[i];
	}

	for (int i = old_num_merged; i--; ) {
		while (num_merged) unmergenodes();
		for (int k = 0; k < CUSTOMERS; k++) {
			assert(orig_cross[k] == cross[k]);
		}
		for (int j = 0; j < CUSTOMERS; j++) {
			cust_order[j] = old_cust_order[j];
		}
		for (int j = 0; j < old_num_merged; j++) if (i != j) {
			if (!in(old_removed_nodes[j],cross[old_merged_nodes[j]])) break;
			mergenodes(old_removed_nodes[j], old_merged_nodes[j]);
		}
		if (num_merged != old_num_merged-1) continue;
//		assert(num_merged == old_num_merged-1);
		removed_nodes[num_merged] = old_removed_nodes[i];
		extendsolution();
		if (!found_solution) {
			if (i < old_num_merged-1) 
				if (verbose) printf("S");
			return;
		}
	}

	while (num_merged) unmergenodes();
	for (int j = 0; j < CUSTOMERS; j++) {
		cust_order[j] = old_cust_order[j];
	}
	for (int j = 0; j < old_num_merged-1; j++) {
		mergenodes(old_removed_nodes[j], old_merged_nodes[j]);
	}
	assert(num_merged == old_num_merged-1);
	removed_nodes[num_merged] = old_removed_nodes[num_merged];
	extendsolution();

//	assert(found_solution);
	assert(num_merged == old_num_merged-1);
}

inline void Node::mergenodes(int a, int b) {
	assert(in(a,cross[b]));
	removed_nodes[num_merged] = a;
	merged_nodes[num_merged] = b;
	old_cross[num_merged] = cross[b];

	for (int i = 0; i < CUSTOMERS; i++) {
		if (in(i,cross[a])) cross[i].addbit(b);
	}
	cross[b] = cross[b] + cross[a];

	mergeset.addbit(a);
	custs.removebit(a);
	custs_open.addbit(a);
	num_relax++;
	num_merged++;
}

inline void Node::unmergenodes() {
	int m = --num_merged;
	int rn = removed_nodes[m];
	int mn = merged_nodes[m];
	SET oc = old_cross[m];

	cross[mn] = oc;
	for (int i = 0; i < CUSTOMERS; i++) {
		if (in(i,oc)) assert(in(mn,cross[i]));
		else cross[i].removebit(mn);
	}

	mergeset.removebit(rn);
	custs.addbit(rn);
	custs_open.removebit(rn);
	num_relax--;
}

inline void Node::finddefinitemove() {

	// Size 1 definite move
	for (int i = 0; i < num_moves; i++) {
		int s = moves[i];
		if (close[s] >= open[s]) {
//			moves[0] = s;
//			num_moves = 1;
			definite[process_id]++;
			dm = true;
		}
	}
					
				
/*
	// Size 2 definite move
	for (int i = 0; i < num_moves; i++) {
		int s = moves[i];
		for (int j = i+1; j < num_moves; j++) {
			int t = moves[j];
			int n = (openset[s] + openset[t]).nbitsp();
			if (n <= close[s] + close[t]) {
				moves[0] = s;
				num_moves = 1;
				definite[process_id]++;
			}
		}
	}
*/
}

inline void Node::prunenotopen() {
	if (!now_open) return;

//	int best_move = num_moves ? moves[0] : -1;

	int s, t;
	for (s = t = 0; s < num_moves; s++) {
		if ((custs_open & closeset[moves[s]]) == emptyset) continue;
		moves[t++] = moves[s];
	}
	num_moves = t;
/*
	if (num_moves == 0 && best_move != -1) {
		moves[num_moves++] = best_move;
	}

	if (num_moves > 0) {
		if (open[moves[0]] > open[best_move]) {
			for (int i = 0; i < num_moves; i++) {
				moves[i+1] = moves[i];
			}
			moves[0] = best_move;
			num_moves++;
		}
	}
*/
}


// Not the most general possible
inline void Node::findoldmoves() {
	if (this == nodes) return;
	if ((this-1)->freeze) return;
	int m = 0;

	for (int i = 0; i < num_moves; i++) {
		int s = moves[i];
		int n = open[s] - (this-1)->close[s] + ((this-1)->custs - custs).nbitsp();
		if (in(s,((this-1)->played)) && n <= free_stacks) {
			if (MYDEBUG) printf("%d - %d %d %d\n", s, open[s], (this-1)->close[s], ((this-1)->custs - custs).nbitsp());
			played = played + closeset[s];
			played_queue[num_played++] = s;
		} else moves[m++] = s;
	}
	num_moves = m;
}


inline void Node::rankmoves() {

	for (int i = 0; i < num_moves; i++) {
		int s = moves[i];
		scores[s] = open[s] - close[s];
//		if (!in(s,custs_open)) scores[s] += CUSTOMERS;
//		if (level && s/25 != nodes[level-1].moves[0]/25) scores[s] += CUSTOMERS;
	}

/*
	double w[num_moves];

	for (int i = 0; i < num_moves; i++) {
		w[i] = 1.0/open[moves[i]];
	}
	for (int i = 0; i < num_moves; i++) {
		int s = moves[i];
		double sum = 0;
		double weight = 0;
		for (int j = 0; j < num_moves; j++) if (i != j) {
			int t = moves[j];
			sum += w[t] * (openset[s]-openset[t]).nbitsp();
			weight += w[t];
		}
		scores[s] = sum / weight - close[s];
	}
*/
}

inline void Node::sortmoves() {
	rankmoves();
	for (int i = 0; i < num_moves; i++) {
		int best_i = i;
		for (int j = i+1; j < num_moves; j++) {
			if (scores[moves[j]] < scores[moves[best_i]]) best_i = j;
		}
		int tmp = moves[i];
		moves[i] = moves[best_i];;
		moves[best_i] = tmp;
	}
}

inline void Node::pruneworsemoves() {
	while (pqhead < num_played) {
		int s = played_queue[pqhead++];
		int m = 0;
		for (int i = 0; i < num_moves; i++) {
			int t = moves[i];
			if (in(t,played)) continue;
			if ((openset[s] + openset[t]).nbitsp() <= close[s] + open[t]) {
				if (MYDEBUG) printf("%d better than %d\n", s, t);
				played.addbit(t);
				played_queue[num_played++] = t;
			} else moves[m++] = t;
		}
		num_moves = m;
	}
}

inline void Node::playmove() {
	Node *next = this+1;
	next->custs_open = custs_open + openset[moves[0]];
	next->custs = custs - closeset[moves[0]];
	next->seconded = seconded + (allopenset[moves[0]] & custs_open);
	next->search();
	played.addbit(moves[0]);
	played_queue[num_played++] = moves[0];
	num_moves--;
	for (int i = 0; i < num_moves; i++) moves[i] = moves[i+1];
}

inline void Node::finish() {
	if (probe && ccc[process_id] >= 100000) return;
  ccc[process_id]++;
	if (showstats && ccc[process_id] % 100000 == 0) printf("sets = %lld, %d [lc=%d,bc=%d,re=%d,dc=%d]\n",ccc[process_id],close_open,loopcount[process_id],bitscount[process_id],reuse[process_id],definite[process_id]);
  if (LIMIT[process_id] && ccc[process_id] >= LIMIT[process_id]) {
		int otime, ttime;
		getrusage(RUSAGE_SELF, &endtime);
		if (!total_optccc) total_optccc = ccc[process_id];
		runtime = endtime;
		opttime = optimaltime;
		otime = 1000 * opttime.ru_utime.tv_sec + opttime.ru_utime.tv_usec/1000
						+ 1000 * opttime.ru_stime.tv_sec + opttime.ru_stime.tv_usec/1000;
		ttime = 1000 * runtime.ru_utime.tv_sec + runtime.ru_utime.tv_usec/1000
						+ 1000 * runtime.ru_stime.tv_sec + runtime.ru_stime.tv_usec/1000;
		printf("%d %d %d %d %lld %d %d %d\n", PRODUCTS, CUSTOMERS, 
		 PRODUCTS, upperbound+1, ccc[process_id], total_optccc, ttime, otime);
    printf("LIMIT %lld EXCEEDED: %s",LIMIT[process_id],name); exit(1);
	}

	if (use_hash && !found_solution && !close_open && !probe) hashset(custs_open & custs,custs);
}


inline void Node::getsolution() {
	int customer_index = 0;

	found_solution = true;

	for (int i = 0; i < level; i++) {
		SET real_closeset = nodes[i].custs - nodes[i+1].mergeset - nodes[i+1].custs;
		for (int j = 0; j < CUSTOMERS; j++) {
			if (!in(j,real_closeset)) continue;
			cust_order[customer_index++] = j;
		}
	}

	if (customer_index != CUSTOMERS - num_relax) printf("%d %d\n", customer_index, CUSTOMERS - num_relax);
	assert(customer_index == CUSTOMERS - num_relax);

	if (MYDEBUG) {
		printf("Customer order: ");
		for (int i = 0; i < customer_index; i++) printf("%d ", cust_order[i]);
		printf("\n");
		for (int i = 0; i <= level; i++) {
			printf("CUSTS OPEN = ");
			printbit(CUSTOMERS,nodes[i].custs_open);
			printf("\nCUSTS      = ");
			printbit(CUSTOMERS,nodes[i].custs);
			printf("\nOPEN SET   = ");
			printbit(CUSTOMERS,nodes[i].openset[nodes[i].moves[0]]);
			printf("\nCLOSE SET  = ");
			printbit(CUSTOMERS,nodes[i].closeset[nodes[i].moves[0]]);
			printf("\n");
		}
	}

}

// Assumes a correct customer order is in custs_order
inline void Node::extendsolution() {
	int n = CUSTOMERS - num_relax;
	cust_order[n-1] = removed_nodes[num_merged];

	SET my_custs_open = emptyset;
	SET my_custs = ALLCUSTOMERS;

	for (int i = 0; i <= level; i++) {
		my_custs = my_custs - nodes[i].mergeset;
	}

	for (int i = 0; i < n; i++) {
		int s = cust_order[i];
		int best_j = i;
		for (int j = i+1; j < n; j++) {
			int t = cust_order[j];
			if ((cross[t]-my_custs_open).subset(cross[s]-my_custs_open)) {
				s = t;
				best_j = j;
			}
		}
		for (int j = best_j; j > i; j--) cust_order[j] = cust_order[j-1];
		cust_order[i] = s;
		my_custs_open = my_custs_open + cross[s];
		if ((my_custs_open & my_custs).nbitsp() > upperbound) {
			found_solution = false;
			return;
		}
		my_custs.removebit(s);
	}

	if (MYDEBUG) {
		printf("Customer order: ");
		for (int i = 0; i < n; i++) printf("%d ", cust_order[i]);
		printf("\n");
	}
//	printf("Successfully extended solution!\n");
}


inline void Node::dumpnode() {
	printf("Level = %d\n", level);
	for (int i = 0; i < num_merged; i++) {
		printf("%d/%d ", removed_nodes[i], merged_nodes[i]);
	}
	printf("\n");
	if (this != nodes) printf("Last move = %d\n", (this-1)->moves[0]);
	printf("Free stacks = %d\n", free_stacks);
	printf("Close_open = %d\n", close_open);
	printf("Custs open = ");
	printbit(CUSTOMERS, custs_open);
	printf("\nCusts      = ");
	printbit(CUSTOMERS, custs);
	printf("\nPlayed     = ");
	printbit(CUSTOMERS, played);
	printf("\n");

	printf("Moves:\n");
	for (int i = 0; i < num_moves; i++) {
		printf("%d: ", moves[i]);
		printbit(CUSTOMERS, openset[moves[i]]);
		printf("\n");
	}

	printf("All:\n");
	for (int i = 0; i < CUSTOMERS; i++) {
		if (!in(i,custs)) continue;
		printf("%d: ", i);
		printbit(CUSTOMERS, openset[i]);
		printf("\n");
	}
}

int getproductorder() {
	int best = 0;
	int product_index = 0;
	SET custs_open = emptyset;
	SET custs = ALLCUSTOMERS;
	bool played[PRODUCTS];

	for (int i = 0; i < PRODUCTS; i++) played[i] = false;

	for (int i = 0; i < CUSTOMERS; i++) {
		custs_open = custs_open + cross[cust_order[i]];
		for (int j = 0; j < PRODUCTS; j++) {
			if (!played[j] && plays[j].subset(custs_open)) {
				played[j] = true;
				prod_order[product_index++] = j;
			}
		}
		int n = (custs_open & custs).nbitsp();
		best = max(best,n);
		custs.removebit(cust_order[i]);
	}
	assert(product_index == PRODUCTS);
	return best;
}

// Expects cust_order, ALLCUSTOMERS and cross to be set correctly
void output() {
  int i,j,b,minprod,maxprod;
  int count[MAXPRODUCTS];

  printf("FINAL\n");
  for (i = 0; i < PRODUCTS; i++) {
    count[i] = 0;
    printf(" %2d ",prod_order[i]);
  }
  printf("\n");
  for (i = 0; i < PRODUCTS; i++) 
    printf("----");
  printf("\n");
  for (j = 0; j < CUSTOMERS; j++) {
    maxprod = -1;
    for (i = 0; i < PRODUCTS; i++) {
      if (prod_order[i] < 0) continue;
      if (playbit[prod_order[i]][j]) maxprod = i;
    }
    minprod = PRODUCTS;
    for (i = 0; i < PRODUCTS; i++) {
      if (prod_order[i] < 0)
				printf("  ? ");
      else {
				if ((b = playbit[prod_order[i]][j])) 
					minprod = i;
				if (b) {
					count[i]++;
//					printf("  X ");
				} else if (i > minprod && i < maxprod) {
					count[i]++;
//				printf("  - ");
				} 
//				else printf("  . ");
      }
    }
//    printf("\n");
  }
//  for (i = 0; i < PRODUCTS; i++) 
//    printf("----");
//  printf("\n");
  for (i = 0; i < PRODUCTS; i++) {
    printf(" %2d ",count[i]);
  }
  printf("\n");
 
}


void readdata(){
  int i,j;
  int tmp;
  long long c;

  scanf("%d",&CUSTOMERS);
  scanf("%d",&PRODUCTS);
  if (CUSTOMERS > MAXCUSTOMERS) {
    printf("ERROR: Too many customers: limit %d\n",MAXCUSTOMERS);
    exit(1);
  }
  if (PRODUCTS > ARRAYPRODUCTS) {
    printf("ERROR: Too many products: limit %d\n",ARRAYPRODUCTS);
    exit(1);
  }
  
  c = 1;
  for (i = 0; i < MAXBIT; i++) {
    bit[i] = c;
    c *= 2;
  }

	for (int i = 0; i < PRODUCTS; i++) plays[i] = emptyset;
	for (int i = 0; i < CUSTOMERS; i++) cross[i] = emptyset;
  

  for (j = 0; j < CUSTOMERS; j++) {
    for (i = 0; i < PRODUCTS; i++) {
      scanf("%d", &tmp);
      playbit[i][j] = tmp;
      if (tmp) plays[i].addbit(j);
    }
  }

	for (int i = 0; i < PRODUCTS; i++) {
		if (plays[i] == emptyset) {
			printf("Warning: Product %d has no customers\n", i);
		}
	}

  for (int i = 0; i < PRODUCTS; i++) {
    for (int j = 0; j < CUSTOMERS; j++) 
      if (in(j,plays[i]))
				cross[j] = cross[j] + plays[i];
  }

	for (int i = 0; i < CUSTOMERS; i++) {
		if (cross[i] == emptyset) {
			printf("ERROR: Customer with no products\n");
			exit(1);
		}
		ALLCUSTOMERS.addbit(i);
		if (MYDEBUG) {
			printbit(CUSTOMERS,cross[i]);
			printf("\n");
		}
	}

	for (int i = 0; i < CUSTOMERS; i++) {
		orig_cross[i] = cross[i];
	}

}

// Looks for solution between upper and lower
int astar(int upper, int lower) {
	int best = upper+1;
	for (int i = upper; i >= lower; i--) {
		upperbound = i;
		best = i+1;
		found_solution = false;
		if (relaxation && !probe) {
//			printf("Relax\n");
			nodes[0].relax();
			close_open = true;
		}
		while (true) {
//			printf("search %d\n", upperbound);
			nodes[0].search();
			if (verbose && found_solution && !close_open) printf("B");
			if (!found_solution) {
				if (!close_open) break;
//				printf("Do full search\n");
				close_open = false;
				continue;
			}
			while (nodes[0].num_merged && found_solution) nodes[0].findunmerge();
			if (!nodes[0].num_merged && found_solution) {
//				printf("Default to no relax search\n");
				break;
			}
		}
		if (found_solution) best = getproductorder();
		if (showstats) if (!probe || found_solution) printf("BACK (%d) asmin = %d, sets = %lld, time = %lld [lc=%d,bc=%d,re=%d,dc=%d]\n",i,best,ccc[process_id],wallClockTime()-mystart,loopcount[process_id],bitscount[process_id],reuse[process_id],definite[process_id]);
		if (!found_solution) break;
		if (best > upperbound) printf("%d %d\n", best, upperbound);
		assert(best <= upperbound);
		i = best;
		getrusage(RUSAGE_SELF,&optimaltime); 
		total_optccc = ccc[process_id];
	}

	return best;
}

int solve() {
	int min = given_upper+1;

	if (probe) {
		min = astar(min-1,given_lower);
		probe = false;
	}

//	printf("End probe\n");
	min = astar(min-1,given_lower);

	return min;
}


void initsolver() {
	nodes[0].custs_open = emptyset;
	nodes[0].custs = ALLCUSTOMERS;
	nodes[0].seconded = emptyset;
}

int main(int argc, char *argv[])
{
  int i;
  char buffer[100];
  char flags[100] = ""; 
  int otime, ttime;
  int min;

	LIMIT[process_id] = 5000000000;

  for (i = 1; i < argc; i++) {
    strcpy(buffer, argv[i]);
    if (buffer[0] == '-') {
      strcat(flags,buffer+1);
      switch(buffer[1]) {
      case 'i':
	/* iteration limit */
	sscanf(argv[i+1],"%lld",&LIMIT[process_id]);
	i++;
	break;
      case 'v':
	/* verbose output */
	verbose = 1;
	break;
      case 't':
	/* show statistics */
	showstats = 1;
	break;
      case 'd':
	/* definite move */
	definite_move = 1;
	break;
      case 'b':
	/* better move */
	better_move = 1;
	break;
      case 'o':
	/* old move */
	old_move = 1;
	break;
      case 'h':
	/* use hash table */
	use_hash = 1;
	break;
      case 'x':
	/* suppress output for benchmarking */
	benchmark = 1;
	break;
      case 'r':
	/* use incomplete search to find true bounds */
	relaxation = 1;
	break;
      case 'p':
	/* probe */
	probe = 1;
	break;
      case 's':
	/* sort moves */
	sort_moves = 1;
	break;
      case 'u':
	/* iteration limit */
	sscanf(argv[i+1],"%d",&given_upper);
	i++;
	break;
      case 'l':
	/* iteration limit */
	sscanf(argv[i+1],"%d",&given_lower);
	i++;
	break;
      default:
	break;
      }
    }
  }

	if (benchmark) {
		verbose = 0;
		showstats = 0;
	}

  //fgets(name,100,stdin);
  getrusage(RUSAGE_SELF, &starttime);
  readdata();
	srand(time(NULL));
	initsolver();
	mystart = wallClockTime();
  min = solve();
	if (verbose) output();
  getrusage(RUSAGE_SELF, &endtime);
  if (!total_optccc) total_optccc = ccc[process_id];
  runtime = endtime;
  opttime = optimaltime;
  otime = 1000 * opttime.ru_utime.tv_sec + opttime.ru_utime.tv_usec/1000
          + 1000 * opttime.ru_stime.tv_sec + opttime.ru_stime.tv_usec/1000;
  ttime = 1000 * runtime.ru_utime.tv_sec + runtime.ru_utime.tv_usec/1000
          + 1000 * runtime.ru_stime.tv_sec + runtime.ru_stime.tv_usec/1000;
	if (benchmark) {
	  printf("%d %d\n", min, ttime);
	} else {
		printf("%d %d %d %d %lld %d %d %d %s %s", PRODUCTS, CUSTOMERS, 
			CUSTOMERS - nodes[0].custs.nbitsp(), min, ccc[process_id], total_optccc, ttime, otime, flags, name);
	}

	if (verbose) {

	}

  exit(0);
}
