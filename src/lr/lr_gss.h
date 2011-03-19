#ifndef _LR_GSS_H_
#define _LR_GSS_H_

namespace lr {
	class gss {
		public:
			/* node merging happens on "producer P at offset O led to state S" identity */
			struct node_id {
				grammar::item::base* P;
				unsigned int O;
				state* S;
				node_id(grammar::item::base*b, unsigned int i, state*s)
					: P(b), O(i), S(s)
				{}
				node_id& operator=(const node_id&n) {
					P = n.P;
					O = n.O;
					S = n.S;
					return *this;
				}
			};

			struct node_id_hash {
				size_t operator()(const node_id*n) const {
					return (0xb4dc0d3 * n->O) * (((char*)n->S) - ((char*)n->P));
				}
			};

			struct node_id_compare {
				size_t operator()(const node_id*a, const node_id*b) const {
					return a->S==b->S && a->O==b->O && a->P==b->P;
				}
			};

			struct node {
				node_id id;
				bool active;
				ast_node_t ast;					/* production */
				node* pred;						/* first ancestor */
				node* link;						/* next in ancestor list of some node */
				node() : id(0, 0, 0), active(0), ast(0), pred(0), link(NULL) {}
				void add_pred(node*p) {
					p->link = pred;
					pred = p;
				}
			};

			struct node_segment {
#				define NODE_SEGMENT_SIZE 1024
				node mypool[NODE_SEGMENT_SIZE];
				node_segment(node_segment* prev=NULL) {
					mypool[0].pred = prev?prev->mypool:NULL;
					for(int i=1;i<NODE_SEGMENT_SIZE;++i) {
						mypool[i].pred = &mypool[i-1];
					}
				}
				node* first() { return mypool; }
			};

			struct node_allocator {
				unsigned int alloc_count;
				node* free_;
				std::vector<node_segment*> segments;
				node_allocator() : alloc_count(0), free_(NULL), segments() {}
				~node_allocator() {
					std::vector<node_segment*>::iterator i, j=segments.end();
					for(i=segments.begin();i!=j;++i) {
						delete *i;
					}
				}
				node* alloc() {
					static unsigned int target=0;
					node* ret;
					if(!free_) {
						node_segment* ns = new node_segment();
						free_ = ns->first();
						segments.push_back(ns);
					}
					ret = free_;
					free_ = free_->pred;
					++alloc_count;
					if(alloc_count>target) {
						std::cout << "gss:: alloc'ed " << alloc_count << " nodes" << std::endl;
						target+=100;
					}
					return ret;
				}
				void free(node* n) {
					n->pred = free_;
					free_ = n;
					--alloc_count;
				}
			} alloc;

			node root;
			std::queue<node*> active;
			item initial;
			ast_node_t accepted;
			unsigned int size;

			typedef ext::hash_map<node_id*, node*, node_id_hash, node_id_compare> gss_node_registry;

			gss_node_registry registry;

			node* alloc_node(node_id&id) {
				node* ret = NULL; //registry[&id];
				if(!ret) {
					ret = alloc.alloc();
					ret->id = id;
					/*registry[&id] = ret;*/
				}
				return ret;
			}

			void free_node(node*n) { registry.erase(&n->id); alloc.free(n); }

			gss(item ini, unsigned int sz) : alloc(), root(), active(), initial(ini), accepted(0), size(sz) {}

			/* TODO : merge only when shifting a non-terminal ? */

			node* shift(node* p, grammar::item::base* producer, state* s, ast_node_t ast, unsigned int offset) {
				if(!p) {
					p=&root;
				}
				node_id id(producer, offset, s);
				node* n = alloc_node(id);
				n->ast = ast;
				n->add_pred(p);
				activate(n);
				return n;
			}

			typedef grammar::rule::internal::append appender;

			/* in stack reduction, i is always at_end() to start with */
			node* reduce(node* n, item i) {
				unsigned int offset = n->id.O;
				/*appender append;*/
				ast_node_t ast;

				/* this is a helper routine which helps track the item path back to the start of the rule in the stack */
				struct {
					bool operator()(node*&p, const grammar::item::base*k) {
						grammar::visitors::lr_item_debugger d;
						while(p) {
							/*std::cout << "comparing "; ((grammar::item::base*)p->id.P)->accept(&d); std::cout << " and "; ((grammar::item::base*)k)->accept(&d); std::cout << std::endl;*/
							if(k->is_same(p->id.P)) {
								/*std::cout << " found matching predecessor !" << std::endl;*/
								return true;
							}
							/*std::cout << " didn't find any matching predecessor." << std::endl;*/
							p = p->link;
						}
						return false;
					}
				} find_pred;

				/* special treatment for epsilon rules */
				if(i.at_start()) {
					/* epsilon rule : reduce ZERO node, and shift from current node. */
					grammar::visitors::reducer red(PRODUCTION_OK_BUT_EMPTY, n->id.O);
					ast_node_t output = red(((grammar::rule::base*)i.rule()));
					std::cout << "epsilon reduction" << std::endl;
					return shift(n, new grammar::item::token::Nt(i.rule()->tag()), n->id.S->transitions.from_stack[i.rule()->tag()], output, n->id.O);
				}

				/* now for the general case, track down the start of the rule in stack */
				const grammar::rule::base* R = i.rule();
				const bool drop_empty = !R->keep_empty();
				--i;
				ast = n->ast==PRODUCTION_OK_BUT_EMPTY ? PRODUCTION_OK_BUT_EMPTY : newPair(n->ast, NULL);
				std::cout << ast << std::endl;
				while(find_pred(n, *i) && (!i.at_start())) {
					--i;
					n = n->pred;
					if(!(drop_empty && n->ast==PRODUCTION_OK_BUT_EMPTY)) {
						if(ast==PRODUCTION_OK_BUT_EMPTY&&drop_empty) {
							ast = n->ast==PRODUCTION_OK_BUT_EMPTY ? PRODUCTION_OK_BUT_EMPTY : newPair(n->ast, NULL);
						} else {
							ast = newPair(n->ast, ast);
						}
						std::cout << ast << std::endl;
					}
				}
				std::cout << ast << std::endl;

				/* if we reach start, we good */
				if(i.at_start()&&find_pred(n, *i)) {	/* if tracking failed, n is NULL, because tracking failed BECAUSE n became NULL. */
					if(initial==i) {
						if(offset != size) {
							std::cout << "can't accept at offset " << offset << " because size is " << size << std::endl;
							return NULL;
						}
						/* accept */
						std::cout << "ACCEPT ! " << ast_serialize_to_string(ast) << std::endl;
						if(ast) {
							grammar::visitors::reducer red(ast, n->id.O);
							ast_node_t output = red((grammar::rule::base*)R);
							accepted = grammar::rule::internal::append()(output, accepted);
						}
						return NULL;
					} else {
						grammar::visitors::reducer red(ast, n->id.O);
						ast_node_t redast = red((grammar::rule::base*)R);
						grammar::item::base* nt = grammar::item::gc(new grammar::item::token::Nt(R->tag()));
						/*std::cout << "Reducing " << ast_serialize_to_string(ast) << " into " << ast_serialize_to_string(redast) << std::endl;*/
						state* Sprime = n->pred->id.S->transitions.from_stack[R->tag()];
						/*state* Sprime = n->id.S->transitions.from_stack[R->tag()];*/
						if(!Sprime) {
							std::cout << "I don't know where to go with a ";
							grammar::visitors::lr_item_debugger d;
							((grammar::rule::base*)R)->accept(&d);
							std::cout << " on top of stack from state : " << std::endl << n->pred->id.S << std::endl;
							/*throw "coin";*/
							return NULL;
						}
						return shift(n->pred, nt, Sprime, redast, offset);
					}
				} else {
					return NULL;
				}
			}
			void activate(node*n) {
				if(!n->active) {
					/*std::cout << "activating node " << n << std::endl;*/
					n->active = true;
					active.push(n);
				}
			}
			node* consume_active() {
				node* ret = active.front();
				ret->active = false;
				active.pop();
				return ret;
			}
	};
}

#endif

