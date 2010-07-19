#include "tinyap_alloc.h"
#include "string_registry.h"

typedef struct _trie_node_t* trie_t;

unsigned long int trie_stats_allocs;
unsigned long int trie_stats_words;

struct _trie_node_t {
	char* radix;
	trie_t next;
	trie_t follow;
	int is_leaf;
};


void trie_dump(trie_t t, int indent) {
	static char buf[4096];
	static int i=0;
	int backup=i;
	if(!indent) {
		i=0;
		memset(buf, 0, 4096);
	}
	if(t->radix) {
		strcpy(buf+i, t->radix);
		i+=strlen(t->radix);
	}
	printf("%*.*s%s %s%s\n", indent*3, indent*3, "", t->radix, t->is_leaf?" > ":"", t->is_leaf?buf:"");
	if(t->follow) {
		trie_dump(t->follow, indent+1);
	}
	i=backup;
	if(t->next) {
		trie_dump(t->next, indent);
	}
}

static inline trie_t trie_alloc() {
	trie_stats_allocs += 1;
	return tinyap_alloc(struct _trie_node_t);
}

trie_t trie_new() {
	trie_t ret = trie_alloc();
	ret->radix=NULL;
	ret->next=NULL;
	ret->follow=NULL;
	ret->is_leaf=1;
	/* stats */
	trie_stats_allocs = 0;
	trie_stats_words = 0;
	return ret;
}


void trie_free(trie_t t) {
	if(!t) { return; }
	trie_free(t->follow);
	trie_free(t->next);
	/*printf("trie_free(%p %s)\n", t, t->radix?t->radix:"<null>");*/
	if(t->radix) { _strfree(t->radix); }
	tinyap_free(struct _trie_node_t, t);
	trie_stats_allocs -= 1;
}


unsigned long match_prefix(const char*s1, const char*s2) {
	const char*base=s1;
	while(*s1&&*s2&&*s1==*s2) { s1+=1; s2+=1; }
	return s1-base;
}


unsigned long trie_match(trie_t t, const char*s) {
	trie_t tmp=t->follow;
	const char* backup = s;
	unsigned long ret=0;
	int l;
	while(tmp) {
		if((l=match_prefix(tmp->radix, s))) {
			if(tmp->radix[l]==0) {
				if(s[l]==0&&tmp->is_leaf) {
					ret = s+l-backup;
				}
				tmp=tmp->follow;
				s+=l;
			} else {
				return 0;
			}
		} else {
			tmp=tmp->next;
		}
	}
	return ret;
}


void trie_insert(trie_t t, const char*s) {
	trie_t cur=t->follow, prev=t;
	int l;

	if(!s) { return; }

	/*printf("trie : inserting %s\n", s);*/

	while(*s&&cur) {
		while(cur&&!(l=match_prefix(cur->radix, s))) {
			cur=cur->next;
		}
		if(!cur) {
			trie_t n = trie_alloc();
			n->follow=NULL;
			n->next=prev->follow;
			prev->follow=n;
			n->radix=_strdup(s);
			n->is_leaf=1;
			trie_stats_words+=1;
			return;
		} else if(cur->radix[l]==0) {
			s+=l;
			prev=cur;
			cur=cur->follow;
		} else {
			trie_t split = trie_alloc();
			split->is_leaf=cur->is_leaf;
			split->radix = _strdup(cur->radix+l);
			split->next=NULL;/*n;*/
			split->follow=cur->follow;
			cur->radix[l]=0;
			/*cur->next=NULL;*/
			/*cur->is_leaf&=!!n;*/
			s+=l;
			cur->follow=split;
			prev=cur;
			cur=NULL;
		}
	}
	if(*s&&!cur) {
		trie_t n = trie_alloc();
		n->radix=_strdup(s);
		n->next=prev->follow;
		n->follow=NULL;
		n->is_leaf=1;
		prev->follow=n;
		trie_stats_words+=1;
	}
}

#ifdef TEST_TRIE

#include <sys/time.h>

double chrono() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return 0.000001*tv.tv_usec+tv.tv_sec;
}

#define EXAMPLE "/usr/share/dict/web2"
/*#define EXAMPLE "web2"*/

int main(int argc, char**argv) {
	trie_t t = trie_new();
	FILE*f = fopen(EXAMPLE, "r");
	char buf[64];
	double t0, t1;
	int counter=0;
	int failures=0;
	t0 = chrono();
	while((!feof(f))&&fscanf(f, "%s", buf)) {
		trie_insert(t, buf);
	}
	fclose(f);
	t1 = chrono();
	printf("fed the beast in %.3lf seconds\n", t1-t0);
	t0 = chrono();
	printf("undertaker => %li\n", trie_match(t, "undertaker"));
	t1 = chrono();
	printf("found the word in %.3lf seconds\n", t1-t0);

	printf("\n%lu allocs for %lu inserts so far (%.3lf allocs/word)\n", trie_stats_allocs, trie_stats_words, trie_stats_allocs/(double)trie_stats_words);

	/*printf("======================================================================================\n");*/
	/*trie_dump(t, 0);*/
	/*printf("======================================================================================\n");*/
	t0 = chrono();
	f = fopen(EXAMPLE, "r");
	while((!feof(f))&&fscanf(f, "%s", buf)) {
		if(strlen(buf)!=trie_match(t, buf)) {
			failures+=1;
			printf("failed to match %s (got %li instead of expected %i)\n", buf, trie_match(t, buf), strlen(buf));
		}
		counter+=1;
	}
	fclose(f);
	t1 = chrono();
	printf("found %i words (failed on %i) in %.3lf seconds\n", counter, failures, t1-t0);

	t0 = chrono();
	trie_free(t);
	t1 = chrono();
	printf("freed the beast in %.3lf seconds\n", t1-t0);
	term_tinyap_alloc();
	return 0;
}

#endif

