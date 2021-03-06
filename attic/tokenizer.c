/* Tinya(J)P : this is not yet another (Java) parser.
 * Copyright (C) 2007 Damien Leroux
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include "config.h"
#include "tinyap.h"
#include "ast.h"
#include "tokenizer.h"
#include "tinyap_alloc.h"
#include "string_registry.h"
#include "serialize.h"

const char* ast_serialize_to_string(const ast_node_t ast);
void delete_node(ast_node_t n);
ast_node_t copy_node(ast_node_t);

size_t hash_str(hash_key k);

const char* op2string(int typ);

int max_rec_level = 0;


ast_node_t SafeAppend(ast_node_t a, ast_node_t b) {
	return a==PRODUCTION_OK_BUT_EMPTY
		? b
		: b==PRODUCTION_OK_BUT_EMPTY
			? a
			: Append(a, b);
}

int dump_node(const ast_node_t n) {
	const char*ptr=tinyap_serialize_to_string(n);
	debug_writeln("%s", ptr);
	free((char*)ptr);
	return 0;
}


void __fastcall update_pos_cache(token_context_t*t);


#define _RE   2
#define _T    4

trie_t token_find_bow(token_context_t*t, char* name) {
	trie_t ret = (trie_t) hash_find(&t->bows, name);
	if(!ret) {
		ret = trie_new();
		hash_addelem(&t->bows, name, ret);
	}
	return ret;
}

void token_bow_add(token_context_t*t, char* name, char* word) {
	trie_insert(token_find_bow(t, name), word);
}

unsigned long token_bow_match(token_context_t*t, char*name) {
	return trie_match_prefix(token_find_bow(t, name), t->source+t->ofs);
}

/* prepends a ^ to reg_expr and returns the compiled extended POSIX regexp */
RE_TYPE token_regcomp(const char*reg_expr) {
	int error_ofs;
	const char* error;
	RE_TYPE initiatur = pcre_compile(reg_expr, 0, &error, &error_ofs, NULL);
	/*fprintf(stderr, "Compiling regex \"%s\"\n", reg_expr);*/
	if(error) {
		fprintf(stderr, "Error : regex compilation of \"%s\" failed (%s at #%i)\n", reg_expr, error, error_ofs);
		return NULL;
	}
	return initiatur;
}

void escape_ncpy(char**dest, char**src, int count, int delim) {
	const char* base=*src;
	while( (*src-base) < count) {
		unescape_chr(src,dest, -1, delim);
	}
}

char* match2str_rpl(const char*repl, const char* match, int n_tok, int* tokens) {
	char* rbuf = _stralloc(strlen(repl)+strlen(match));
	char*dest=rbuf;
	char*src=(char*)repl,*subsrc;

	while(*src) {
		if(*src=='\\'&& *(src+1)>='0' && *(src+1)<='9') {
			int n = *(src+1)-'0';
			subsrc = (char*) (match+tokens[2*n]);
			escape_ncpy(&dest,&subsrc, tokens[2*n+1]-tokens[2*n], '/');
			src+=2;
		} else {
			unescape_chr(&src,&dest, 8/*no context*/, '/');
		}
	}
	*dest=0;
	/*return _strdup(rbuf);*/
	return rbuf;
	/*return regstr(rbuf);*/
}



char*match2str(const char*src,const size_t start,const size_t end) {
	char* buf = _stralloc(end-start+1);
	char* rd = (char*)src+start;
	char* wr = buf;
	size_t sz=end-start-1,ofs=0;

	if(end>start) {
//	printf("match2str orig = \"%*.*s\" sz=%li\n",(int)(end-start),(int)(end-start),rd,sz);
//		memset(buf,0,end-start);
//	printf("              => \"%s\"\n",buf);
		while(ofs<sz) {
			unescape_chr(&rd, &wr, _RE, '/');
			ofs = rd-src-start;
//		printf("match2str orig = \"%*.*s\"\n",(int)(end-start-ofs),(int)(end-start-ofs),rd);
//		printf("              => \"%s\" %p %p %li\n",buf,rd,buf,ofs);
		};
		if(ofs==sz) {
			*wr = *rd;
			wr += 1;
		}
	}
	*wr = 0;

//	printf("match2str => \"%s\"\n\n",buf);

//	static char buf[256];
//	memset(buf,0,256);
//	strncpy(buf,src+start,end-start);
	return buf;
}






static inline void token_context_enter_raw(token_context_t*t, int raw) {
	push(t->raw_stack, (void*)raw);
}


static inline void token_context_leave_raw(token_context_t*t) {
	_pop(t->raw_stack);
}


token_context_t*token_context_new(const char*src,const size_t length,const char*garbage_regex,ast_node_t greuh,size_t drapals) {
	token_context_t*t=(token_context_t*)malloc(sizeof(token_context_t));
	t->source=strdup(src);
	t->ofs=0;
	t->size=length;
	/*t->ofsp=0;*/
	t->ofstack = new_stack();
	t->flags=drapals;
	if(garbage_regex) {
		t->garbage=token_regcomp(garbage_regex);
		printf("t->garbage = \"%s\" %p\n",garbage_regex, t->garbage);
	} else {
		printf("t->garbage = NULL\n");
		t->garbage=NULL;
	}
	t->grammar=greuh;			/* grou la grammaire */
	t->farthest=0;
	t->farthest_stack = new_stack();
	t->node_stack = new_stack();
	t->pos_cache.last_ofs=0;
	t->pos_cache.last_nlofs=0;
	t->pos_cache.row=1;
	t->pos_cache.col=1;
	t->raw_stack = new_stack();

	node_cache_init(t->cache);

	init_hashtab(&t->bows, (hash_func)hash_str, (compare_func)strcmp);

	t->expected=NULL;

	token_context_enter_raw(t, 0);

	return t;
}

int node_compare(ast_node_t tok1, ast_node_t tok2) {
	if(tok1==tok2) {
		return 0;
	}
	if(isNil(tok1)) {
		return isNil(tok2)?0:-1;
	} else if(isNil(tok2)) {
		return 1;
	} else if(isAtom(tok1)) {
		char* n;
		return isPair(tok2)
			? 1
			: isAtom(tok2)
				? strcmp((n=Value(tok1))<((char*)0x100) ? op2string((int)n) : n,
					 (n=Value(tok2))<((char*)0x100) ? op2string((int)n) : n)
				: 0;
	} else if(isPair(tok1)) {
		if(isPair(tok2)) {
			int ret = node_compare(Car(tok1), Car(tok2));
			return ret?ret:node_compare(Cdr(tok1), Cdr(tok2));
		} else {
			return 1;
		}
	}

	return tok1>tok2?1:-1;
}



void __fastcall token_expected_at(token_context_t*t, ast_node_t expr) {
	ast_node_t tmp;
	if(t->farthest>t->ofs) {
		return;
	}
	if(t->farthest<t->ofs) {
		delete_node(t->expected);
		t->expected=NULL;
		t->farthest=t->ofs;
	}
	for(tmp=t->expected;tmp&&node_compare(Car(tmp), expr);tmp=Cdr(tmp));
	if(!(tmp&&Car(tmp))) {
		t->expected = newPair(copy_node(expr), t->expected, 0, 0);
	}
}


static inline unsigned long token_context_peek(const token_context_t*t) {
	return (unsigned long) _peek(t->ofstack);
	/*return t->ofstack[t->ofsp-1];*/
}



static inline void token_context_push(token_context_t*t, const char*tag) {
	push(t->ofstack, (void*)t->ofs);
	/*push(t->node_stack,(void*)tag);*/
	printf("  [%lu] << %-10.10s%s", t->ofstack->sp, t->source+t->ofs,((int)t->ofs)<(((int)strlen(t->source))-10)?"... >>\n":" >>\n");
}

extern int tinyap_verbose;

static inline void token_context_validate(token_context_t*t, ast_node_t result) {
	static int last = 0;
	/* TODO : implement node caching here */
	/*t->ofsp-=1;*/		/* release space on stack, don't update t->ofs */
	_pop(t->ofstack);
	if(t->farthest<t->ofs && !(t->flags&INPUT_IS_CLEAN)) { 	/* just matched a token, so we didn't expect an unmet token. */
		/*t->farthest=t->ofs;*/
		/*free_stack(t->farthest_stack);*/
		/*t->farthest_stack = stack_dup(t->node_stack);*/
		/*delete_node(t->expected);*/
		/*t->expected=NULL;*/
	}
	/*_pop(t->node_stack);*/
	printf("  [%lu]  => OK! ofs=%u\t\t\t\t%s\n", t->ofstack->sp, t->ofs, tinyap_serialize_to_string(result));
	t->flags&=~INPUT_IS_CLEAN;
	if(tinyap_verbose&&(last>>10)!=(t->ofs>>10)) {
		last=t->ofs;
		fprintf(stderr, "%u / %u    \r", t->ofs, t->size);
	}
	/*printf("input is unclean.\n");*/
}



static inline void token_context_pop(token_context_t*t) {
	/*if(failed_expr && t->farthest==t->ofs) {*/
		/*char* tag = Value(Car(failed_expr));*/
		/*if(!(TINYAP_STRCMP(tag, STR_T)&&TINYAP_STRCMP(tag, STR_RE)&&TINYAP_STRCMP(tag, STR_RPL))) {*/
			/*t->expected = newPair(copy_node(failed_expr), t->expected, 0, 0);*/
		/*}*/
	/*}*/
	/*_pop(t->node_stack);*/
	/*printf("  [%lu]  => FAIL!\n", t->ofstack->sp);*/
	/*if(!t->ofsp) {*/
		/*return;*/
	/*}*/
	/*t->ofsp-=1;*/
	/*t->ofs=t->ofstack[t->ofsp];*/
	t->ofs = (unsigned long) _pop(t->ofstack);
}



void htab_clean_bow(htab_entry_t e) {
	_strfree((char*)e->key);
	trie_free((trie_t)e->e);
}

void token_context_free(token_context_t*t) {
	if(t->garbage) {
		/*regfree(t->garbage);*/
		/*tinyap_free(regex_t, t->garbage);*/
		pcre_free(t->garbage);
	}
	delete_node(t->grammar);
	free_stack(t->ofstack);
	free_stack(t->raw_stack);
	free_stack(t->node_stack);
	free_stack(t->farthest_stack);
	/*node_cache_flush(t->cache);*/
	clean_hashtab(&t->bows, htab_clean_bow);
	free(t->source);
	free(t);
}




/*
 * basic production rule from regexp : [garbage]token_regexp
 * return NULL on no match
 */

/*ast_node_t __fastcall token_produce_re(token_context_t*t,const RE_TYPE expr) {*/
ast_node_t __fastcall token_produce_re(token_context_t*t, ast_node_t re) {
	int token[3];
	char*lbl;
	int r,c;
	ast_node_t ret=NULL;
	const RE_TYPE expr;
	re = Car(Cdr(re));
	/* perform some preventive garbage filtering */
	/*_filter_garbage(t);*/
	/*update_pos_cache(t);*/
	if(!re->raw._p2) {
		/* take advantage of unused atom field to implement regexp cache */
		re->raw._p2=token_regcomp(Value(re));
	}
	expr = re->raw._p2;
	r=t->pos_cache.row;
	c=t->pos_cache.col;
	/*if(regexec(expr,t->source+t->ofs,1,&token,0)!=REG_NOMATCH&&token.rm_so==0) {*/
	/*if(regexec_hack(expr,t,1,&token,0)!=REG_NOMATCH&&token.rm_so==0) {*/
	if(re_exec(expr, t, token, 3)) {
		/*assert(token.rm_so==0);*/
		lbl=match2str(t->source+t->ofs,0,token[1]);
		t->ofs+=token[1];
		/*printf("debug-- matched token [%s]\n",lbl);*/
		update_pos_cache(t);
		ret = newPair(newAtom(lbl,t->pos_cache.row,t->pos_cache.col),NULL,r,c);
		_strfree(lbl);
		//return newAtom(lbl,t->pos_cache.row,t->pos_cache.col);
	} else {
		token_expected_at(t, re);
		/*debug_write("debug-- no good token\n");*/
	}
	return ret;
}



ast_node_t __fastcall token_produce_delimstr(token_context_t*t, ast_node_t str) {
	char* ret = NULL;
	char* _src;
	char* _end;
	char* _match;
	ast_node_t delim = Cdr(str);
	char* prefix=Value(Car(delim));
	char* suffix=Value(Cadr(delim));
	size_t slen = strlen(prefix);
	/*printf(__FILE__ ":%i\n", __LINE__);*/
	if(*prefix&&strncmp(t->ofs+t->source,prefix,slen)) {
		return NULL;
	}
	/*printf(__FILE__ ":%i\n", __LINE__);*/
	_src = t->ofs+t->source+slen;
	if(!*suffix) {
		/*printf(__FILE__ ":%i\n", __LINE__);*/
		_match = ret = _stralloc(t->source+t->size-_src+1);
		escape_ncpy(&_match, &_src, t->source+t->size-_src, -1);
		t->ofs = t->size;
	} else {
		/*printf(__FILE__ ":%i\n", __LINE__);*/
		_end = _src;
		/*printf(__FILE__ ":%i\n", __LINE__);*/
		while((_match=strchr(_end, (int)*suffix))&&_match>_end&&*(_match-1)=='\\') {
			/*printf(__FILE__ ":%i\n", __LINE__);*/
			_end = _match+1;
		}
		/*printf(__FILE__ ":%i\n", __LINE__);*/
		if(!_match) {
			token_expected_at(t, str);
			return NULL;
		}
		/*printf(__FILE__ ":%i\n", __LINE__);*/
		_end = _match;
		ret = _stralloc(_end-_src+1);
		_match = ret;
		escape_ncpy(&_match, &_src, _end-_src, (int)*suffix);
		*_match=0;
		t->ofs = _end-t->source+1;
	}
	/*printf(__FILE__ ":%i\n", __LINE__);*/
	return newPair(newAtom(ret, t->pos_cache.row, t->pos_cache.col), NULL, t->pos_cache.row, t->pos_cache.col);
}


#if 0
/*
 * basic production rule from regexp+replacement : [garbage]token_regexp
 * return NULL on no match
 */

ast_node_t __fastcall token_produce_rpl(token_context_t*t,const RE_TYPE expr, const char*rplc) {
	int tokens[30];
	char*lbl;
	int r,c;
	ast_node_t ret=NULL;
	/* perform some preventive garbage filtering */
	/*_filter_garbage(t);*/
	update_pos_cache(t);
	r=t->pos_cache.row;
	c=t->pos_cache.col;
	/*if(regexec(expr,t->source+t->ofs,10,tokens,0)!=REG_NOMATCH&&(*tokens).rm_so==0) {*/
	/*if(regexec_hack(expr,t,10,tokens,0)!=REG_NOMATCH&&(*tokens).rm_so==0) {*/
	if(re_exec(expr, t, tokens, 30)) {
		lbl=match2str_rpl(rplc,t->source+t->ofs,10,tokens);
		ret = newPair(newAtom(lbl,t->pos_cache.row,t->pos_cache.col),NULL,r,c);
		/*debug_write("debug-- replaced to %s [%s]\n",rplc,lbl);*/
		_strfree(lbl);
		/*t->ofs+=(*tokens).rm_eo;*/
		t->ofs+=tokens[1];
		update_pos_cache(t);
		//return newAtom(lbl,t->pos_cache.row,t->pos_cache.col);
//	} else {
//		debug_write("debug-- no good token\n");
	}
	return ret;
}
#endif

ast_node_t __fastcall token_produce_bow(token_context_t*t,ast_node_t bow) {
	ast_node_t bow_data = Cdr(bow);
	unsigned long slen = token_bow_match(t, Value(Car(bow_data)));
	if(slen>0) {
		ast_node_t ret;
		if(!Cdr(bow_data)) {
			ret = PRODUCTION_OK_BUT_EMPTY;
		} else {
			char*tok = _stralloc(slen+1);
			strncpy(tok, t->source+t->ofs, slen);
			tok[slen]=0;
			ret = newPair(	newAtom(tok, t->pos_cache.row, t->pos_cache.col),
					NULL, t->pos_cache.row, t->pos_cache.col);
		}
		t->ofs+=slen;
		/*update_pos_cache(t);*/
		return ret;
	}
	token_expected_at(t, bow);
	return NULL;
}


ast_node_t __fastcall token_produce_str(token_context_t*t, ast_node_t expr) {
	/*int r,c;*/
	size_t slen;
	const char*token=Value(Car(Cdr(expr)));
	/*_filter_garbage(t);*/
	update_pos_cache(t);
	/*r=t->pos_cache.row;*/
	/*c=t->pos_cache.col;*/
	slen=strlen(token);
	if(!strncmp(t->source+t->ofs,token,slen)) {
		t->ofs+=slen;
		update_pos_cache(t);
		return PRODUCTION_OK_BUT_EMPTY;
		/*return t->flags&STRIP_TERMINALS*/
			/*? PRODUCTION_OK_BUT_EMPTY*/
			/*: newPair(newAtom(token,t->pos_cache.row,t->pos_cache.col),NULL,r,c);*/
		//return newAtom(token,t->pos_cache.row,t->pos_cache.col);
	} else {
		token_expected_at(t, expr);
	}
	return NULL;
}

/*

elem = /.../
T ::= "\"" /.../ "\""
NT ::= "<" /.../ ">"
RE ::= "/" /.../ "/"

rule = ( <opr_rule> | <trans_rule> ) .

opr_rule ::= <Elem> "::=" <rule_expr> "." .
trans_rule ::= <Elem> "=" <rule_expr> "." .

rule_expr = ( "(" <alt> ")" | <seq> ) .

seq ::= ( <rule_elem> <rule_seq> | <rule_elem> ) .

alt ::= ( <rule_seq> "|" <alt> | <rule_seq> ) .

rule_elem = ( <TermID> | <NTermID> | <RegexID> | "(" <alt> ")" ) .

_start=(<Rule> <_start> | <rule>).

#
# The AST is defined by the parser result with all terminal tokens stripped
#
#

*/



ast_node_t __fastcall token_produce_any(token_context_t*t,ast_node_t expr, ast_node_t follow);




#define node_tag(_x) Value(Car(_x))
#define node_cdr(_x) Value(Car(Cdr(_x)))



ast_node_t __fastcall find_nterm(const ast_node_t ruleset,const char*ntermid) {
	ast_node_t root=getCar(ruleset);
	ast_node_t n=getCdr(root);	/* skip tag */
	assert(!TINYAP_STRCMP(Value(getCar((ast_node_t )root)),STR_Grammar));	/* and be sure it made sense */
//	dump_node(n);
//	printf("\n");
	while(n&&((!TINYAP_STRCMP(node_tag(getCar(n)), STR_Comment))||TINYAP_STRCMP(node_tag(getCdr(getCar(n))),ntermid))) {	/* skip operator tag to fetch rule name */
//		debug_writeln("skip rule ");
//		dump_node(getCar(n));
		n=getCdr(n);
	}
	if(n) {
//		debug_writeln("FIND_NODE SUCCESSFUL\n");
//		dump_node(getCar(n));
		return getCar(n);
	}
	return NULL;
}


ast_node_t __fastcall _produce_seq_rec(token_context_t*t,ast_node_t seq, ast_node_t follow) {
	static int rec_lvl=0;
	ast_node_t tmp=NULL, rec, rec_tmp, _cdr, rec_cdr;

	ast_node_t debug=seq;

	/*printf("<< %10.10s%s", t->source+t->ofs,t->ofs<(strlen(t->source)-10)?"... >>":" >>   ");*/
	/*printf("  ENTERING SEQ "); dump_node(debug); printf("\n");*/

	/* if seq is Nil, don't fail */
	if(!seq) {
		return NULL;
	}

	/* try and produce first token */
	tmp=token_produce_any(t,getCar(seq), getCdr(seq));

	if(tmp) {
		/* try and produce rest of list */
		/*printf("seq:: start seq %s ; first production is %s\n", tinyap_serialize_to_string(seq), tinyap_serialize_to_string(tmp));*/
		if(tmp!=PRODUCTION_OK_BUT_EMPTY) {
			_cdr = tmp;
			while(Cdr(_cdr)) { _cdr = Cdr(_cdr); }
		} else {
			_cdr = NULL;
		}
		seq = Cdr(seq);

		while(seq&&(rec=token_produce_any(t, Car(seq), Cdr(seq)))) {
			if(rec!=PRODUCTION_OK_BUT_EMPTY) {
#if 0
				if(rec->node_flags&IS_FOREST) {
					ast_node_t check, prev;
					/* switch to recursive mode to handle forests */
					rec_tmp = rec;
					prev=NULL;
					printf("  SEQ in forest %s, prefix=%s\n", tinyap_serialize_to_string(rec), tmp?tinyap_serialize_to_string(tmp):"null");
					while(rec_tmp) {
						token_context_push(t, NULL);
						t->ofs = rec_tmp->pos.col;
						/*update_pos_cache(t);*/
						printf(" SEQ FOREST %s << %10.10s%s", tinyap_serialize_to_string(rec_tmp), t->source+t->ofs,t->ofs<(strlen(t->source)-10)?"... >>":" >>   ");
						check = _produce_seq_rec(t, Cdr(seq));
						if(check) {
							printf("  SEQ in forest at %s, prefix=%s, check=%s\n", tinyap_serialize_to_string(Car(rec_tmp)), tmp?tinyap_serialize_to_string(tmp):"null", check?tinyap_serialize_to_string(check):"FAIL!");
							if(check!=PRODUCTION_OK_BUT_EMPTY) {
								if(tmp!=PRODUCTION_OK_BUT_EMPTY) {
									/* insert seq prefix at beginning of result */
									ast_node_t prefix = copy_node(tmp);
									rec_cdr=prefix;
									while(Cdr(rec_cdr)) { rec_cdr=Cdr(rec_cdr); }
									Cdr(rec_cdr)=Car(rec_tmp);
									Car(rec_tmp) = prefix;
								}
								/* append rest of result */
								rec_cdr=Car(rec_tmp);
								while(Cdr(rec_cdr)) { rec_cdr=Cdr(rec_cdr); }
								Cdr(rec_cdr) = check;
							}
							rec_tmp->pos.col = t->ofs;
							rec_tmp = Cdr(rec_tmp);
						} else {
							if(prev) {
								Cdr(prev) = Cdr(rec_tmp);
								delete_node(rec_tmp);
								rec_tmp = Cdr(prev);
							} else {
								rec = Cdr(rec_tmp);
								delete_node(rec_tmp);
								rec_tmp = rec;
							}

						}
						token_context_pop(t);
					}
					if(rec) {
						delete_node(tmp);
						if(!Cdr(rec)) {
							rec_tmp = Car(rec);
							Car(rec)=NULL;
							delete_node(rec);
							rec = rec_tmp;
							/*printf("  SEQ deforestified\n");*/
						/*} else {*/
							/*printf("  SEQ forest final => %s\n", tinyap_serialize_to_string(rec));*/
						}
						if(_cdr) {
							_cdr->pair._cdr = rec;
						} else {
							tmp = rec;
							_cdr = rec;
						}
					}
					return rec;
				} else
#endif
				{	/* if !IS_FOREST */
					update_pos_cache(t);
					if(_cdr) {
						_cdr->pair._cdr = rec;
					} else {
						tmp = rec;
						_cdr = rec;
					}
					while(_cdr->pair._cdr) { _cdr = _cdr->pair._cdr; }
				}
			}
			if(seq&&!(rec->node_flags&IS_FOREST)) {
				seq = Cdr(seq);
			} else { /* if it's a forest, then follow has been processed */
				seq = NULL;
			}
			/*printf("seq:: now tmp=%p _cdr=%p seq=%p\n", tmp, _cdr, seq);*/
		}

		if(seq) {
			/*printf("seq:: ended with remaining %s ; had produced %s\n", tinyap_serialize_to_string(seq), tinyap_serialize_to_string(tmp));*/
			/*abort();*/
			/*delete_node(tmp);*/
			/*printf("  SEQ %s => FAIL!\n", tinyap_serialize_to_string(debug));*/
			return NULL;
		} else {
			/*printf("  SEQ %s => %s\n", tinyap_serialize_to_string(debug), tinyap_serialize_to_string(tmp));*/
			return tmp;
		}
		
#if 0
		rec=_produce_seq_rec(t,getCdr(seq));
		if(rec) {
			update_pos_cache(t);
			if(isAtom(rec)) {
				assert(!TINYAP_STRCMP(Value(rec),STR_eos));
				(void)(rec?delete_node(rec):0);
				//return newPair(tmp,NULL,t->pos_cache.row,t->pos_cache.col);
				return tmp;
			} else {
				//return newPair(tmp,rec,t->pos_cache.row,t->pos_cache.col);
				return SafeAppend(tmp,rec);
			}
		} else {
			/* FIXME : delay deletions until final cache flush */
			//delete_node(tmp);
			return NULL;
		}
#endif
	}
	return NULL;
}



ast_node_t  __fastcall token_produce_seq(token_context_t*t,ast_node_t seq, ast_node_t follow) {
	ast_node_t ret;

	/* try and produce seq */
	ret=_produce_seq_rec(t,seq, follow);
/*	if(ret) {
		return newPair(ret,NULL,0,0);
	} else {
		return NULL;
	}*/
	return ret;
}



ast_node_t  __fastcall token_produce_alt(token_context_t*t,ast_node_t alt, ast_node_t follow) {
	static int rec_lvl = 0;
	/* FIXME : apparently returns FAIL when supposed to return EMPTY */
	if(t->flags&FULL_PARSE) {
		ast_node_t tmp, bak=NULL, empty=NULL, tmp_head, spa=NULL;

		ast_node_t debug=alt;

		rec_lvl+=1;
		/*printf("  [%i] << %10.10s%s", rec_lvl, t->source+t->ofs,t->ofs<(strlen(t->source)-10)?"... >>":" >>   ");*/
		/*printf(" ENTERING ALT"); dump_node(debug); printf("\n");*/
		while(alt) {
			token_context_push(t, NULL);
			/*printf("  ALT ofs=%u\n", t->ofs);*/
			/*printf("  [%i] ALT testing %s\n", rec_lvl, tinyap_serialize_to_string(Car(alt)));*/
			tmp=token_produce_any(t,Car(alt), NULL);
			if(tmp) {
				if(tmp==PRODUCTION_OK_BUT_EMPTY) {
					/*rec_lvl-=1;*/
					/*empty=PRODUCTION_OK_BUT_EMPTY;*/
					/*return token_produce_any(t, follow, NULL);*/
					empty = ((!follow)||token_produce_any(t, follow, NULL))?PRODUCTION_OK_BUT_EMPTY:NULL;
					if(empty) {
						/*printf("  [%i] EMPTY PRODUCTION OK !\n", rec_lvl);*/
					}
				} else {
					tmp_head = tmp;
					while(Cdr(tmp)) { tmp = Cdr(tmp); }
					if((!follow)||(spa=token_produce_seq(t, follow, NULL))) {
						#if 1
						bak=newPair(NULL, bak, 0, t->ofs);
						if(tmp_head==PRODUCTION_OK_BUT_EMPTY) {
							Car(bak) = spa;		/* can't append unless the caller is notified that follow has been processed */
						} else if(spa==PRODUCTION_OK_BUT_EMPTY) {
							Car(bak) = tmp_head;
						} else {
							Cdr(tmp) = spa;		/* can't append unless the caller is notified that follow has been processed */
							/*delete_node(spa);*/		/* FIXME : add to cache */
							Car(bak) = tmp_head;
						}
						#else
						bak=newPair(tmp_head, bak, 0, t->ofs);
						#endif
						bak->node_flags|=IS_FOREST;
					}
				}
			}
			alt = Cdr(alt);
			token_context_pop(t);
		};
		/*printf("  [%i] ALT %s   =>   got %s%s\n", rec_lvl, tinyap_serialize_to_string(debug), empty?"EMPTY | ":"", tinyap_serialize_to_string(bak));*/
		if(empty&&bak) {
			bak = newPair(PRODUCTION_OK_BUT_EMPTY, bak, 0, t->ofs);
		}
		if(bak&&!Cdr(bak)) {
			tmp=Car(bak);
			Car(bak)=NULL;
			t->ofs = bak->pos.col;
			delete_node(bak);
			/*printf("  ALT unforestified\n");*/
			rec_lvl-=1;
			return tmp;
		}
		/*printf("  [%i] ALT %s => %s\n", rec_lvl, tinyap_serialize_to_string(debug), bak?tinyap_serialize_to_string(bak):empty?"EMPTY":"FAIL!");*/
		rec_lvl-=1;
		return bak?bak:empty;
	#if 0
		if(bak) {
			unsigned int ofs = bak->pos.col;
			tmp = Car(bak);
			printf("initially select %s ofs=%u\n", tinyap_serialize_to_string(tmp), ofs);
			if(Cdr(bak)) {
				printf("Ambiguity detected...\n\t%s\n", tinyap_serialize_to_string(bak));
				bak=Cdr(bak);
				do {
					printf("testing ofs %u\n", Car(bak)->pos.col);
					if(bak->pos.col>ofs) {
						tmp = Car(bak);
						ofs = tmp->pos.col;
						printf("select %s ofs=%u\n", tinyap_serialize_to_string(tmp), ofs);
					}
					bak=Cdr(bak);
				} while(bak);
				printf("\tkeeping %s\n", tinyap_serialize_to_string(tmp));
			}
			t->ofs = ofs;
			return tmp;
		}
		return NULL;
	#endif
	} else {
		ast_node_t tmp;

		while(alt&&!(tmp=token_produce_any(t,getCar(alt), NULL))) {
			alt = getCdr(alt);
		};
		return tmp;
	}
}






/*
 * détection des récursions à gauche simples :
 * si la règle est une alternative
 * et
 * si le premier item (en descendant dans alt et seq) de la règle est (NT règle) 
 * et
 * si la règle est une alternative
 * et
 * s'il y a au moins une alternative non left-réentrante :
 * 	- on teste les autres alternatives d'abord (postpone).
 * 	- sinon :
 *	 	- on teste si le reste de la règle parse au niveau de récurrence 0.
 * 		- si oui, on teste au niveau 1, et ainsi de suite jusqu'à foirer.
 *	- on retourne le dernier résultat non foiré (s'il en est)
 * RESTRICTION :
 * 	la règle doit être de la forme (Alt (Seq (NT règle) ...) (?)) avec (?) ne commençant pas par (NT règle)
 *
 * il faudrait détecter toutes les récurrences, surtout celles pas gérées, avec une recherche dans une pile d'ops.
 */
ast_node_t blacklisted=NULL, replacement=NULL;

ast_node_t __fastcall token_produce_leftrec(token_context_t*t,ast_node_t expr,int isOp, ast_node_t follow) {
	const char*tag = node_tag(Cdr(expr));
	ast_node_t
		tmp = Cdr(Car(Cdr(Cdr(expr)))),
		alt1 = Car(tmp),
		alt2 = Car(Cdr(tmp));

	/*printf("alt1 = %s\nalt2 = %s\n",tinyap_serialize_to_string(alt1),tinyap_serialize_to_string(alt2));*/
	tmp = token_produce_any(t,alt2, NULL);
	if(tmp&&isOp) {
		tmp=newPair(newPair(newAtom(tag,0,0),tmp,0,0),NULL,0,0);
	}

	if(tmp) {
		blacklisted=Car(Cdr(alt1));
		do {
			replacement = tmp;
			tmp = token_produce_any(t,alt1, NULL);
			if(tmp&&isOp) {
				tmp=newPair(newPair(newAtom(tag,0,0),tmp,0,0),NULL,0,0);
			}
			/*printf("prout %s\n",tinyap_serialize_to_string(tmp));*/
		} while(tmp);
		tmp = replacement;
		blacklisted=NULL;
	}
	return tmp;
}




int __fastcall check_trivial_left_rec(ast_node_t node) {
	static ast_node_t last=NULL;
	const char*tag=node_tag(Cdr(node));
	ast_node_t alt;

	if(node==last) {
		// don't re-detect, it's being handled
		return 0;
	}
	last=node;

	if(node->node_flags&RULE_IS_LEFTREC_COMPUTED) {
		return node->node_flags&RULE_IS_LEFTREC;
	}

	node->node_flags |= RULE_IS_LEFTREC_COMPUTED;
	node->node_flags &= ~RULE_IS_LEFTREC;

/* 	la règle doit être de la forme (Alt (Seq (NT règle) ...) (?)) avec (?) ne commençant pas par (NT règle) */
	//printf("check lefty %s\n",tinyap_serialize_to_string(node));
	ast_node_t elems=Car(Cdr(Cdr(node)));

	if(isAtom(elems)) {
		return 0;
	}

	//printf("\t%s\n",node_tag(elems));
	if(!TINYAP_STRCMP(node_tag(elems),STR_Alt)) {
		alt=elems;
		elems=Cdr(elems);
		//printf("\t%s\n",node_tag(Car(elems)));
		if(!TINYAP_STRCMP(node_tag(Car(elems)),STR_Seq)) {
			elems=Cdr(Car(elems));
			//printf("\t%s\n",node_tag(Car(elems)));
			if(!TINYAP_STRCMP(node_tag(Car(elems)),STR_NT)) {
				//printf("\t%s %s\n",node_tag(Cdr(Car(elems))),tag);
				if(!TINYAP_STRCMP(node_tag(Cdr(Car(elems))),tag)) {
					/* get second part of alternative */
					elems=Cdr(Cdr(alt));
					if(elems) {
						elems=Cdr(elems);
						//printf("!elems => %i\n",!elems);
						node->node_flags|=(!elems)?RULE_IS_LEFTREC:0;
						return !elems;/* 0 if more than 2 parts in alternative, 1 if exactly 2 */
					}
				}
			}
		}
	}
	return 0;
}



#define OP_EOF       1
#define OP_RE        2
#define OP_T         3
#define OP_RTR       4
#define OP_ROP       5
#define OP_PREFX     6
#define OP_NT        7
#define OP_SEQ       8
#define OP_ALT       9
#define OP_POSTFX   10
#define OP_RAWSEQ   11
#define OP_REP_0N   12
#define OP_REP_01   13
#define OP_REP_1N   14
#define OP_EPSILON  15
#define OP_RPL      16
#define OP_STR      17
#define OP_BOW      18
#define OP_ADDTOBAG 19
#define OP_BKEEP    20

const char* op2string(int typ) {
	switch(typ) {
	case OP_EOF: return STR_EOF;
	case OP_RE: return STR_RE;
	case OP_T: return STR_T;
	case OP_STR: return STR_STR;
	case OP_BOW: return STR_BOW;
	case OP_ADDTOBAG: return STR_AddToBag;
	case OP_BKEEP: return STR_AddToBag;
	case OP_RTR: return STR_TransientRule;
	case OP_ROP: return STR_OperatorRule;
	case OP_PREFX: return STR_Prefix;
	case OP_NT: return STR_NT;
	case OP_SEQ: return STR_Seq;
	case OP_ALT: return STR_Alt;
	case OP_POSTFX: return STR_Postfix;
	case OP_RAWSEQ: return STR_RawSeq;
	case OP_REP_0N: return STR_Rep0N;
	case OP_REP_01: return STR_Rep01;
	case OP_REP_1N: return STR_Rep1N;
	case OP_EPSILON: return STR_Epsilon;
	case OP_RPL: return STR_RPL;
	default: return NULL;
	};
}


int string2op(const char* tag) {
	int typ=0;
	if(!TINYAP_STRCMP(tag,STR_Seq)) {
		typ = OP_SEQ;
	} else if(!TINYAP_STRCMP(tag,STR_BOW)) {
		typ = OP_BOW;
	} else if(!TINYAP_STRCMP(tag,STR_BKeep)) {
		typ = OP_BKEEP;
	} else if(!TINYAP_STRCMP(tag,STR_AddToBag)) {
		typ = OP_ADDTOBAG;
	} else if(!TINYAP_STRCMP(tag,STR_RawSeq)) {
		typ = OP_RAWSEQ;
	} else if(!TINYAP_STRCMP(tag,STR_Rep0N)) {
		typ = OP_REP_0N;
	} else if(!TINYAP_STRCMP(tag,STR_Rep1N)) {
		typ = OP_REP_1N;
	} else if(!TINYAP_STRCMP(tag,STR_Rep01)) {
		typ = OP_REP_01;
	} else if(!TINYAP_STRCMP(tag,STR_Alt)) {
		typ = OP_ALT;
	} else if(!TINYAP_STRCMP(tag,STR_RE)) {
		typ = OP_RE;
	} else if(!TINYAP_STRCMP(tag,STR_RPL)) {
		typ = OP_RPL;
	} else if(!TINYAP_STRCMP(tag,STR_T)) {
		typ = OP_T;
	} else if(!TINYAP_STRCMP(tag,STR_STR)) {
		typ = OP_STR;
	} else if(!TINYAP_STRCMP(tag,STR_NT)) {
		typ = OP_NT;
	} else if(!TINYAP_STRCMP(tag,STR_Prefix)) {
		typ = OP_PREFX;
	} else if(!TINYAP_STRCMP(tag,STR_Postfix)) {
		typ = OP_POSTFX;
	} else if(!TINYAP_STRCMP(tag,STR_TransientRule)) {
		typ = OP_RTR;
	} else if(!TINYAP_STRCMP(tag,STR_OperatorRule)) {
		typ = OP_ROP;
	} else if(!TINYAP_STRCMP(tag,STR_EOF)) {
		typ = OP_EOF;
	} else if(!TINYAP_STRCMP(tag,STR_Epsilon)) {
		typ = OP_EPSILON;
	}
	return typ;
}


ast_node_t __fastcall token_produce_any(token_context_t*t,ast_node_t expr, ast_node_t follow) {
//	static int prit=0;
	static int rec_lvl=0;
	char*tag;
	char*key=NULL;
	char*err_tag=NULL;
	ast_node_t ret=NULL, pfx=NULL, tmp=NULL;
	int typ=0;
	int r,c;
	size_t dummy;
	int row,col;
	ast_node_t nt, re;
	const ast_node_t debug=expr, debug_f = follow;
	/*ast_node_t fail = NULL;*/

	if(!expr) {
		/*return NULL;*/
		return PRODUCTION_OK_BUT_EMPTY;
	}

	// trivial left-recursion handling
	if(expr==blacklisted) {
		//return newPair(newAtom("strip.me",0,0),NULL,0,0);
		return replacement;
	}

	_filter_garbage(t);
	update_pos_cache(t);

	row = t->pos_cache.row;
	col = t->pos_cache.col;

#if 0
	if(isAtom(expr)) {
		if(!TINYAP_STRCMP(Value(expr),STR_Epsilon)) {
			/*return newPair(newAtom(STR_strip_me,0,0),NULL,row,col);*/
			return PRODUCTION_OK_BUT_EMPTY;
		} else if(!TINYAP_STRCMP(Value(expr),STR_EOF)) {
			_filter_garbage(t);
			update_pos_cache(t);
			if(t->ofs<t->size) {
				/*fprintf(stderr, "EOF not matched at #%u (against #%u) ATOM\n",t->ofs,t->size);*/
				ret=NULL;
			} else {
				/*fprintf(stderr, "EOF matched at #%u (against #%u) ATOM\n",t->ofs,t->size);*/
				/*ret=newPair(newAtom(STR_strip_me,t->pos_cache.row,t->pos_cache.col),NULL,t->pos_cache.row,t->pos_cache.col);*/
				ret = PRODUCTION_OK_BUT_EMPTY;
			}
			return ret;
		/*} else {*/
			/*printf("%s\n",tinyap_serialize_to_string(expr));*/
		}
	}
#endif

	tag=node_tag(expr);
	if(Car(expr)->node_flags&ATOM_IS_NOT_STRING) {
		typ = (int)tag;
	} else {
		typ = string2op(tag);
		Value(Car(expr)) = (void*)typ;
		Car(expr)->node_flags |= ATOM_IS_NOT_STRING;
	}
	switch(typ) {
		case OP_EPSILON:
			return PRODUCTION_OK_BUT_EMPTY;
		case OP_RE:
		case OP_RPL:
		case OP_STR:
		/*case OP_ROP:*/
			key = Value(Car(Cdr(expr)));
		default:;	
	};
#if 0
	switch(typ) {
		case OP_RPL:
		case OP_ROP:
		case OP_T:
			err_tag = Value(Car(Cdr(expr)));
		default:;	
	};
	switch(typ) {
		case OP_T:
		case OP_RE:
		case OP_RPL:
			fail = expr;
		default:;
	};
#endif

#if 1
	printf("     %s  %s\n", tinyap_serialize_to_string(expr), tinyap_serialize_to_string(follow));
	/*printf("[%i] << %10.10s%s %s\n", rec_lvl, t->source+t->ofs,t->ofs<(strlen(t->source)-10)?"... >>":" >>   ", tinyap_serialize_to_string(expr));*/
#endif


//*
	if(key&&node_cache_retrieve(t->cache, row, col, key, &ret,&t->ofs)) {
		/*fprintf(stderr,"found %s at %i:%i %s\n",key,row, col,tinyap_serialize_to_string(ret));*/
		update_pos_cache(t);
		return ret==PRODUCTION_OK_BUT_EMPTY?ret:copy_node(ret);	/* keep PROD.. a singleton */
	}
//*/

	rec_lvl+=1;
	max_rec_level = rec_lvl>max_rec_level?rec_lvl:max_rec_level;

	token_context_push(t,err_tag);

	switch(typ) {
	case OP_SEQ:
		token_context_enter_raw(t, 0);
		ret=token_produce_seq(t,getCdr(expr), NULL);
		token_context_leave_raw(t);
		break;
	case OP_RAWSEQ:
		token_context_enter_raw(t, 1);
		ret=token_produce_seq(t,getCdr(expr), NULL);
		token_context_leave_raw(t);
		break;
	case OP_ALT:
		ret=token_produce_alt(t,getCdr(expr), NULL);
		break;
	case OP_RE:
		re = getCar(getCdr(expr));
		key = Value(re);
		ret=token_produce_re(t, expr);
		break;
	case OP_RPL:
		re = getCar(getCdr(expr));
		key = Value(re);
		/*printf("match \"%s\" / replace \"%s\"\n",key,Value(Car(Cdr(Cdr(expr)))));*/
		/*ret=token_produce_rpl(t,re->raw._p2,Value(getCar(getCdr(getCdr(expr)))));*/
		ret=PRODUCTION_OK_BUT_EMPTY;
		break;
	case OP_T:
		ret=token_produce_str(t,expr);
//		debug_write("### -=< term %s >=- ###\n",ret?"OK":"failed");
		break;
	case OP_STR:
		ret=token_produce_delimstr(t, expr);
//		debug_write("### -=< term %s >=- ###\n",ret?"OK":"failed");
		break;
	case OP_BOW:
		ret = token_produce_bow(t, expr);
		break;
	case OP_ADDTOBAG:
		expr = Cdr(expr);
		ret = token_produce_any(t, Car(expr), NULL);
		if(ret) {
			expr=Cdr(expr);
			token_bow_add(t, Value(Car(expr)), Value(Car(ret)));
			expr=Cdr(expr);
			if(!expr) {
				ret = PRODUCTION_OK_BUT_EMPTY;
			}
		}
		break;
	case OP_ROP:
/*
 * détection des récursions à gauche simples :
 * si la règle est une alternative
 * et
 * si le premier item (en descendant dans alt et seq) de la règle est (NT règle) 
 * et
 * si la règle est une alternative
 * et
 * s'il y a au moins une alternative non left-réentrante :
 * 	- on teste les autres alternatives d'abord (postpone).
 * 	- sinon :
 *	 	- on teste si le reste de la règle parse au niveau de récurrence 0.
 * 		- si oui, on teste au niveau 1, et ainsi de suite jusqu'à foirer.
 *	- on retourne le dernier résultat non foiré (s'il en est)
 * RESTRICTION :
 * 	la règle doit être de la forme (Alt (Seq (NT règle) ...) (?)) avec (?) ne commençant pas par (NT règle)
 *
 * il faudrait détecter toutes les récurrences, surtout celles pas gérées, avec une recherche dans une pile d'ops.
 */
		if(check_trivial_left_rec(expr)) {
			ret = token_produce_leftrec(t,expr,1, NULL);
			tag=node_tag(Cdr(expr));
		} else {
			expr=getCdr(expr);	/* shift the operator tag */
	//		dump_node(expr);
			update_pos_cache(t);
			r=t->pos_cache.row;
			c=t->pos_cache.col;
			tag=node_tag(expr);

			ret=token_produce_any(t,getCar(getCdr(expr)), NULL);
			if(ret) {
				ret=newPair(newPair(newAtom(tag,r,c),ret,r,c),NULL,r,c);
	//			debug_write("Produce OperatorRule ");
	//			dump_node(expr);
	//			dump_node(ret);
	//			fputc('\n',stdout);
	//			printf("add to cache [ %li:%li:%s ] %p\n", t->pos_cache.row, t->pos_cache.col, tag, ret);
	//			node_cache_add(t->cache, t->pos_cache.row, t->pos_cache.col, tag, ret);
			}
		}
		break;
	case OP_RTR:
		if(check_trivial_left_rec(expr)) {
			ret = token_produce_leftrec(t,expr,0, NULL);
		} else {
			expr=getCdr(expr);	/* shift the operator tag */
	//		dump_node(expr);
			tag=node_tag(expr);

			ret=token_produce_any(t,getCar(getCdr(expr)), NULL);
		}
		break;
	case OP_REP_01:
		pfx = token_produce_any(t,getCar(getCdr(expr)), NULL);
		if(pfx!=NULL) {
			ret = pfx;
		} else {
			/*ret = newPair(newAtom(STR_strip_me,0,0), NULL,t->pos_cache.row,t->pos_cache.col);*/
			ret = PRODUCTION_OK_BUT_EMPTY;
		}
		break;
	case OP_REP_1N:
		pfx = token_produce_any(t,getCar(getCdr(expr)), NULL);
		if(pfx!=NULL&&pfx!=PRODUCTION_OK_BUT_EMPTY) {
			unsigned long last_ofs = t->ofs;
			char*stmp = (char*) tinyap_serialize_to_string(expr);
			char*stmp2 = (char*)tinyap_serialize_to_string(pfx);
			update_pos_cache(t);
			printf("got prefix for rep 1,N for expr %s at %i,%i : %s\n",stmp,t->pos_cache.row,t->pos_cache.col,stmp2);
			free(stmp);
			free(stmp2);
			/*ret = pfx;*/
			while( (tmp = token_produce_any(t,getCar(getCdr(expr)), NULL))
						&&
					tmp != PRODUCTION_OK_BUT_EMPTY
						&&
					last_ofs!=t->ofs ) {
				last_ofs=t->ofs;
				/*stmp = (char*) tinyap_serialize_to_string(tmp);*/
				printf("    continue for rep 1,N at %i,%i : %s\n",t->pos_cache.row,t->pos_cache.col,stmp);
				/*free(stmp);*/
				#if 0
				while(pfx->pair._cdr) {
					pfx=pfx->pair._cdr;
				}
				pfx->pair._cdr = tmp;
				pfx = tmp;
				#endif
				pfx = forest_append(pfx, tmp);
			}
			ret = pfx;
		}
		break;
	case OP_REP_0N:
		pfx = token_produce_any(t,getCar(getCdr(expr)), NULL);
		if(pfx!=NULL&&pfx!=PRODUCTION_OK_BUT_EMPTY) {
			unsigned long last_ofs = t->ofs;
			/*ret = pfx;*/
			while( (tmp = token_produce_any(t,getCar(getCdr(expr)), NULL))
						&&
					tmp != PRODUCTION_OK_BUT_EMPTY
						&&
					last_ofs!=t->ofs  ) {
				#if 0
				last_ofs=t->ofs;
				while(pfx->pair._cdr) {
					pfx=pfx->pair._cdr;
				}
				pfx->pair._cdr = tmp;
				pfx = tmp;
				#endif
				pfx = forest_append(pfx, tmp);
			}
			ret = pfx;
		} else {
			ret = PRODUCTION_OK_BUT_EMPTY;
		}
		break;
	case OP_PREFX:
		expr=getCdr(expr);	/* shift the operator tag */
//		dump_node(expr);
		//tag=node_tag(expr);

		pfx=token_produce_any(t,getCar(expr), NULL);
		if(pfx!=NULL) {
			/*printf("have prefix %s\n",tinyap_serialize_to_string(pfx));*/
			ret=token_produce_any(t,getCar(getCdr(expr)), NULL);
			if(ret==PRODUCTION_OK_BUT_EMPTY) {
				return pfx;
			} else if(pfx!=PRODUCTION_OK_BUT_EMPTY &&
				  ret && ret->pair._car) {
				ast_node_t tail;
				/* these copies are necessary because of structural hack.
				 * Not copying botches the node cache.
				 */
				ret=copy_node(ret);
				pfx=copy_node(pfx);

				tail=pfx;
				/*printf("have expr %s\n",tinyap_serialize_to_string(ret));*/
				//ret->pair._car->pair._cdr = Append(pfx,ret->pair._car->pair._cdr);
				/* FIXME ? Dirty hack. */
				while(tail->pair._cdr) {
					tail = tail->pair._cdr;
				}
				tail->pair._cdr = ret->pair._car->pair._cdr;
				ret->pair._car->pair._cdr = pfx;
				/*printf("\nhave merged into %s\n\n",tinyap_serialize_to_string(ret));*/
			}
		}
		break;
	case OP_POSTFX:
		expr=getCdr(expr);	/* shift the operator tag */
//		dump_node(expr);
		//tag=node_tag(expr);

		pfx=token_produce_any(t,getCar(expr), NULL);
		if(pfx!=NULL) {
			//printf("have postfix %s\n",tinyap_serialize_to_string(pfx));
			ret=token_produce_any(t,getCar(getCdr(expr)), NULL);
			if(ret&&ret->pair._car) {
				//printf("have expr %s\n",tinyap_serialize_to_string(ret));
				//ret->pair._car->pair._cdr = Append(pfx,ret->pair._car->pair._cdr);
				ret->pair._car = SafeAppend(ret->pair._car,pfx);

				//printf("have merged into %s\n",tinyap_serialize_to_string(ret));
			}
		}
		break;
	case OP_NT:
		tag = Value(getCar(getCdr(expr)));
		if(!(TINYAP_STRCMP(tag, STR_Space) && TINYAP_STRCMP(tag, STR_NewLine) && TINYAP_STRCMP(tag, STR_Indent) && TINYAP_STRCMP(tag, STR_Dedent))) {
			/*ret=newPair(newAtom(STR_strip_me,t->pos_cache.row,t->pos_cache.col),NULL,t->pos_cache.row,t->pos_cache.col);*/
			ret = PRODUCTION_OK_BUT_EMPTY;
		} else {
			if(!node_cache_retrieve(t->cache,0,0,tag,&nt,&dummy)) {
				nt=find_nterm(t->grammar,tag);
				node_cache_add(t->cache,0,0,tag,nt,0);
			}
		
			if(!nt) {
				/* error, fail */
				debug_write("FAIL-- couldn't find non-terminal `%s'\n", tag);
				ret=NULL;
			} else {
				ret=token_produce_any(t,nt, NULL);
			}
		}
		break;
	case OP_EOF:
		_filter_garbage(t);
		//if(*(t->source+t->ofs)&&t->ofs!=t->length) {
		if(t->ofs<t->size) {
			/*fprintf(stderr, "EOF not matched at #%u (against #%u)\n",t->ofs,t->size);*/
			ret=NULL;
		} else {
			/*fprintf(stderr, "EOF matched at #%u (against #%u)\n",t->ofs,t->size);*/
			/*update_pos_cache(t);*/
			/*ret=newPair(newAtom(STR_strip_me,t->pos_cache.row,t->pos_cache.col),NULL,t->pos_cache.row,t->pos_cache.col);*/
			ret = PRODUCTION_OK_BUT_EMPTY;
		}
		break;
	};

	rec_lvl-=1;
	if(ret) {
		/* add to node cache */
		if(key) {
			fprintf(stderr,"add to cache [ %i:%i:%s ] %s\n", row, col, key, tinyap_serialize_to_string(ret));
			node_cache_add(t->cache,row,col,key,ret,t->ofs);
		}
		//dump_node(ret);
		token_context_validate(t, ret);
		printf("     %s  %s   => OK!\n", tinyap_serialize_to_string(expr), tinyap_serialize_to_string(follow));
		return ret;
	} else {
		token_context_pop(t);
		printf("     %s  %s   => FAIL!\n", tinyap_serialize_to_string(expr), tinyap_serialize_to_string(follow));
		return NULL;
	}
}






ast_node_t __fastcall clean_ast(ast_node_t t) {
	if(!t) {
		return NULL;
	}
	if(isAtom(t)) {
		if(TINYAP_STRCMP(Value(t),STR_strip_me)) {
			return t;
		} else {
			delete_node(t);
			return NULL;
		}
	} else if(isPair(t)) {
		t->pair._car=clean_ast(t->pair._car);
		t->pair._cdr=clean_ast(t->pair._cdr);
		//if(t->pair._car==NULL&&t->pair._cdr==NULL) {
		if(t->pair._car==NULL) {
			ast_node_t cdr=t->pair._cdr;
			t->pair._cdr=NULL;
			delete_node(t);
			return cdr;
		}
	}
	return t;
}



void __fastcall update_pos_cache(token_context_t*t) {
	int ln=t->pos_cache.row;		/* line number */
	size_t ofs=t->pos_cache.last_ofs;
	size_t end=t->ofs;
	size_t last_nlofs=t->pos_cache.last_nlofs;

	if(ofs==end) {
		return;
	}

	if(t->ofs<t->pos_cache.last_ofs) {
		while(ofs>end) {
			if(t->source[ofs]=='\n') {
				--ln;
			}
			--ofs;
		}
		while(ofs>0&&t->source[ofs]!='\n') {
			--ofs;
		}
		last_nlofs=ofs+(!!ofs);	/* don't skip character if at start of buffer */
	} else {
		while(ofs<end) {
			if(t->source[ofs]=='\n') {
				++ln;
				last_nlofs=ofs+1;
			}
			++ofs;
		}

	}

	if(ln>t->pos_cache.row) {
		t->pos_cache.row=ln;
		t->pos_cache.col=1+end-last_nlofs;
		t->pos_cache.last_ofs=end;
		t->pos_cache.last_nlofs=last_nlofs;
		/*node_cache_clean(t->cache, &t->pos_cache);*/
	}
	t->pos_cache.row=ln;
	t->pos_cache.col=1+end-last_nlofs;
	t->pos_cache.last_ofs=end;
	t->pos_cache.last_nlofs=last_nlofs;
	/*printf("update_pos_cache now ofs=%i/%i (%i lines)\n", end, t->size, t->pos_cache.row);*/
}


const char* parse_error(token_context_t*t) {
	static char err_buf[4096];
	size_t last_nlofs=0;
	size_t next_nlofs=0;
	size_t tab_adjust=0;
	const char* expected;
	int i;
	char*sep,*k;

	t->ofs=t->farthest;
	update_pos_cache(t);
	last_nlofs=t->ofs-t->pos_cache.col+1;
	
	next_nlofs=last_nlofs;
	while(t->source[next_nlofs]&&t->source[next_nlofs]!='\n') {
		if(t->source[next_nlofs]=='\t') {
			tab_adjust+=8-((next_nlofs-last_nlofs)&7);	/* snap to tabsize 8 */
		}
		next_nlofs+=1;
	}

	err_buf[0]=0;

	/*if((long)t->farthest_stack->sp>=0) {*/
		/*sep = " In context ";*/
/**/
		/*for(i=0;i<=(long)t->farthest_stack->sp;i+=1) {*/
			/*k=(char*)t->farthest_stack->stack[i];*/
			/*if(k) {*/
				/*strcat(err_buf,sep);*/
				/*strcat(err_buf,k);*/
				/*sep=".";*/
			/*}*/
		/*}*/
/**/
		/*strcat(err_buf,",\n");*/
	/*}*/

	expected = ast_serialize_to_string(t->expected);

//	sprintf(err_buf,"parse error at line %i :\n%*.*s\n%*.*s^\n",
	sprintf(err_buf+strlen(err_buf),"%*.*s\n%*.*s^\nExpected one of %s",
		(int)(next_nlofs-last_nlofs),
		(int)(next_nlofs-last_nlofs),
		t->source+last_nlofs,
		(int)(t->farthest-last_nlofs+tab_adjust),
		(int)(t->farthest-last_nlofs+tab_adjust),
		"",
		expected
	);

	free((char*)expected);

	return err_buf;
}

int parse_error_column(token_context_t*t) {
	t->ofs=t->farthest;
	update_pos_cache(t);
	return t->pos_cache.col;
}

int parse_error_line(token_context_t*t) {
	t->ofs=t->farthest;
	update_pos_cache(t);
	return t->pos_cache.row;
}

