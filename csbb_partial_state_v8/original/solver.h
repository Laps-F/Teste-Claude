#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <assert.h>
#include <new>
#include <time.h>
//#include <glib.h>

static inline long long wallClockTime(void) {
	struct timeval tp;
	gettimeofday(&tp, NULL);
	return (long long) tp.tv_sec * 1000000 + tp.tv_usec;}

/******************************* MULTI THREADING CHANGES ****************************/
#define MAXTHREAD 32


long long ccc[MAXTHREAD];
int loopcount[MAXTHREAD];
int bitscount[MAXTHREAD];
int reuse[MAXTHREAD];
int definite[MAXTHREAD];
long long LIMIT[MAXTHREAD];

int process_id = 0;

int total_ccc = 0;
int total_optccc = 0;
int total_loopcount = 0;
int total_bitscount = 0;
int total_reuse = 0;
int total_definite = 0;
int total_LIMIT = 0;

#define ARRAYPRODUCTS 256
#define MAXPRODUCTS   ARRAYPRODUCTS
#define MAXCUSTOMERS  ARRAYPRODUCTS
#define MAXBIT 64

/******************************* CLASS DEFINITION ****************************/
/*
class SET {
public:
  long long high, low;
	SET() {}
	SET(long long h, long long l) : high(h), low(l) {}

	bool operator == (SET S) const { return (high == S.high) && (low == S.low); }
	bool operator != (SET S) const { return (high != S.high) || (low != S.low); }
	bool operator < (SET S) const { return (high < S.high) || ((high == S.high) && (low < S.low)); }
	SET operator + (SET S) const { return SET(high | S.high, low | S.low); }
	SET operator - (SET S) const { return SET(high & ~S.high, low & ~S.low); }
	SET operator & (SET S) const { return SET(high & S.high, low & S.low); }

	void addbit(int i) { if (i < 64) low |= ((long long) 1 << i); else high |= ((long long) 1 << (i-64)); }
	void removebit(int i) { if (i < 64) low &= ~((long long) 1 << i); else high &= ~((long long) 1 << (i-64)); }
	bool subset(SET S) { return (low == (low & S.low)) && (high == (high & S.high)); }

	int nbitsp() const {
		int c;
		long long word;
		for (c = 0, word = high; word; c++) word &= word - 1; // clear the least significant bit set
		for (word = low; word; c++)word &= word - 1; // clear the least significant bit set
		bitscount[process_id]++;
		return c;
	}

};
*/

class SET {
public:
  long long a[4];
	SET() {}
	SET(long long a0, long long a1, long long a2, long long a3) {
		a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3;
	}

	bool operator == (SET S) const {
		return a[0] == S.a[0] && a[1] == S.a[1] && a[2] == S.a[2] && a[3] == S.a[3];
	}
	bool operator != (SET S) const {
		return a[0] != S.a[0] || a[1] != S.a[1] || a[2] != S.a[2] || a[3] != S.a[3];
	}

	bool operator < (SET S) const {
		if (a[3] < S.a[3]) return true;
		if (a[3] > S.a[3]) return false;
		if (a[2] < S.a[2]) return true;
		if (a[2] > S.a[2]) return false;
		if (a[1] < S.a[1]) return true;
		if (a[1] > S.a[1]) return false;
		if (a[0] < S.a[0]) return true;
		return false;
	}

	SET operator + (SET S) const {
		return SET(a[0] | S.a[0], a[1] | S.a[1], a[2] | S.a[2], a[3] | S.a[3]);
	}
	SET operator - (SET S) const {
		return SET(a[0] & ~S.a[0], a[1] & ~S.a[1], a[2] & ~S.a[2], a[3] & ~S.a[3]);
	}
	SET operator & (SET S) const {
		return SET(a[0] & S.a[0], a[1] & S.a[1], a[2] & S.a[2], a[3] & S.a[3]);
	}

	void addbit(int i) {
		a[i/64] |= ((long long) 1 << (i%64));
	}
	void removebit(int i) {
		a[i/64] &= ~((long long) 1 << (i%64));
	}
	bool subset(SET S) {
		return (a[0] == (a[0] & S.a[0])) && (a[1] == (a[1] & S.a[1])) &&
			(a[2] == (a[2] & S.a[2])) && (a[3] == (a[3] & S.a[3]));
	}

	int nbitsp() const {
		int c = 0;
		long long word;
		for (word = a[0]; word; c++) word &= word - 1; // clear the least significant bit set
		for (word = a[1]; word; c++) word &= word - 1; // clear the least significant bit set
		for (word = a[2]; word; c++) word &= word - 1; // clear the least significant bit set
		for (word = a[3]; word; c++) word &= word - 1; // clear the least significant bit set
		bitscount[process_id]++;
		return c;
	}

	bool in(int i) const {
		return ((a[i/64] & ((long long) 1 << (i%64))) != 0);
	}

};


const SET emptyset = SET(0,0,0,0);

//#define in(e,S) ( (e) < 64 ? bit[e] & S.low : bit[(e)-64] & S.high ) 
#define in(e,S) (S.in(e)) 

long long bit[64];

#define max(a,b)  ((a) > (b) ? (a) : (b) )

/******************************* LINEAR CHAINING ****************************/

//#define TABLESIZE        33554432  /* 2^25 */
#define TABLESIZE        16777216  /* 2^24 */
#define MAXENTRIES       TABLESIZE

int collisions = 0;
int hash_entries = 0;

typedef struct entry {
  SET p;
	SET q;
	int time_stamp;
  struct entry *next;
} ENTRY, *EP;

EP hsh[TABLESIZE];

#define mix(a,b,c) \
{ \
  a -= b; a -= c; a ^= (c>>13); \
  b -= c; b -= a; b ^= (a<<8); \
  c -= a; c -= b; c ^= (b>>13); \
  a -= b; a -= c; a ^= (c>>12);  \
  b -= c; b -= a; b ^= (a<<16); \
  c -= a; c -= b; c ^= (b>>5); \
  a -= b; a -= c; a ^= (c>>3);  \
  b -= c; b -= a; b ^= (a<<10); \
  c -= a; c -= b; c ^= (b>>15); \
}

unsigned int ghash(SET p) {
   register unsigned int a,b,c;

   a = b = 0x9e3779b9;    /* the golden ratio; an arbitrary value */
   c = 392842354;

	 a += p.a[0];
   b += (p.a[0]>>32);
   mix(a,b,c);
   c += p.a[1];
   a += (p.a[1]>>32);
   mix(a,b,c);

   return c;
}

void printarr(int SIZE, int a[])
{
  int i;
  for (i = 0; i < SIZE; i++)
    printf("%5d",a[i]);
  printf("\n");
}

void printbit(int nbits, SET n)
{
  int  i;
  for (i = nbits-1; i >= 0; i--)
    if (in(i,n)) printf("1");
    else printf("0");
}

int hash(SET p, SET q) {
  int i;
  EP  r;

#ifdef DEBUG
  printf("hash (");
  printbit(CUSTOMERS,p);
  printf(")=");
#endif

//	i = p.low & 0x07ffffff;
//	i = (unsigned long long) p.low % 332500027;
//	i = (ghash(p)>>7);
	i = (ghash(p)>>8);

	assert(i >= 0);
	assert(i < TABLESIZE);
  /* printf("h %d\n",i); */
  r = hsh[i];
  while (r && (r->p != p))
    r = r->next;
#ifdef DEBUG
  if (!r) printf("0\n");
  else printf("%d\n",r->opt);
#endif

	if (r) {
		while (r && (r->p == p)) {
			if ((r->q).subset(q)) {
				r->time_stamp = ccc[process_id];
				return 1;
			}
			r = r->next;
		}
	}
	return 0;
}

void hashset(SET p, SET q) {
  int i, j;
  EP  r, last = NULL;

	hash_entries++;

	if (hash_entries%100000 == 0) printf("Hash entries: %d\n", hash_entries);

	if (hash_entries >= MAXENTRIES) {
		printf("Clean hash table\n"); fflush(stdout);
		for (j = 0; j < TABLESIZE; j++) {
			EP t = hsh[j];
			while (t) {
				if (t->time_stamp < ccc[process_id]-MAXENTRIES/2) {
					hsh[j] = t->next;
					free(t);
					hash_entries--;
					t = hsh[j];
				} else break;
			}
			if (t) {
				EP prev = t;
				EP cur = t->next;
				while (cur) {
					if (cur->time_stamp < ccc[process_id]-MAXENTRIES/2) {
						prev->next = cur->next;
						free(cur);
						hash_entries--;
						cur = prev->next;
					} else {
						prev = cur;
						cur = cur->next;
					}
				}
			}
		}
		printf("Remaining entries: %d\n", hash_entries); fflush(stdout);
	}


#ifdef DEBUG
  printf("hashset (");
  printbit(CUSTOMERS,p);
#endif

//	i = p.low & 0x07ffffff;
//	i = (unsigned long long) p.low % 332500027;
//	i = (ghash(p)>>7);
	i = (ghash(p)>>8);

	assert(i >= 0 && i < TABLESIZE);
  /*  printf("hs %d\n",i); */
  r = hsh[i];

	if (r) collisions++;

	while (r && (r->p != p)) {
		last = r;
    r = r->next;
	}
  if (!r) {
    r = (EP) malloc(sizeof(ENTRY));
    r->p = p;
		r->q = q;
		r->time_stamp = ccc[process_id];
    r->next = hsh[i];
    hsh[i] = r;
	} else {
		while (r && (r->p == p)) {
			if (q.subset(r->q)) {
				EP tmp = r;
				r = r->next;
				free(tmp);
				if (last) last->next = r;
				else hsh[i] = r;
			} else {
				last = r;
				r = r->next;
			}
		}
	}

  return;
}


void flushhashtable() {
	for (int i = 0; i < TABLESIZE; i++) {
		EP r = hsh[i];
		while (r) {
			EP next = r->next;
			free(r);
			r = next;
		}
		hsh[i] = NULL;
	}
}
