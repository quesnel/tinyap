#include "lr.h"

extern "C" {
	const char* ast_serialize_to_string(ast_node_t);
}

namespace grammar {
namespace item {

namespace token {
	bool Nt::is_same(const base* i) const {
		if(!i) {
			return !this;
		}
		if(class_id()==i->class_id()) {
			return equal_to<Nt>()(
						dynamic_cast<const Nt*>(this),
						dynamic_cast<const Nt*>(i));
		}
		const grammar::rule::base* R = dynamic_cast<const grammar::rule::base*>(i);
		if(R) {
			return tag()==R->tag();
		}
		return false;
	}
} /* namespace token */

	ext::hash_map<const ast_node_t, base*, lr::hash_an, lr::ptr_eq<_ast_node_t> > registry;

	struct clean_registry_at_exit {
		~clean_registry_at_exit() {
			ext::hash_map<const ast_node_t, base*, lr::hash_an, lr::ptr_eq<_ast_node_t> >::iterator
				i, j=registry.end();
			for(i=registry.begin();i!=j;++i) {
				delete (*i).second;
			}
		}
	} _clean_registry_at_exit;

	base* base::from_ast(const ast_node_t n, Grammar* g) {
		base* cached = registry[n];
		if(cached) {
			/*std::cout << "reusing cached item ";*/
			/*visitors::debugger d;*/
			/*cached->accept(&d);*/
			/*std::cout << std::endl;*/
			return cached;
		} else {
			/*std::cout << "no cached item for node " << ast_serialize_to_string(n) << std::endl;*/
			registry[n] = cached;
		}
		visitors::item_rewriter rw(g);
		const char* tag = Value(Car(n));
		if(tag==STR_RE) {
			cached = new token::Re(Value(Car(Cdr(n))));
		} else if(tag==STR_T) {
			cached = new token::T(Value(Car(Cdr(n))));
		} else if(tag==STR_EOF) {
			cached = new token::Eof();
		} else if(tag==STR_Epsilon) {
			cached = new token::Epsilon();
		} else if(tag==STR_Comment) {
			cached = new token::Comment(Value(Car(Cdr(n))));
		} else if(tag==STR_NT) {
			cached = new token::Nt(Value(Car(Cdr(n))));
		} else if(tag==STR_Alt) {
			combination::Alt* edit = new combination::Alt();
			ast_node_t m=Cdr(n);
			while(m) {
				base* i = rw(Car(m));
				if(i) { edit->insert(i); }
				m=Cdr(m);
			}
			cached = edit;
		} else if(tag==STR_RawSeq) {
			combination::RawSeq* edit = new combination::RawSeq();
			ast_node_t m=Cdr(n);
			while(m) {
				base* i = rw(Car(m));
				if(i) { edit->push_back(i); }
				m=Cdr(m);
			}
			cached = edit;
		} else if(tag==STR_Seq) {
			combination::Seq* edit = new combination::Seq();
			ast_node_t m=Cdr(n);
			while(m) {
				base* i = rw(Car(m));
				if(i) { edit->push_back(i); }
				m=Cdr(m);
			}
			cached = edit;
		} else if(tag==STR_Rep0N) {
			cached = new combination::Rep0N(rw(Car(Cdr(n))));
		} else if(tag==STR_Rep01) {
			cached = new combination::Rep01(rw(Car(Cdr(n))));
		} else if(tag==STR_Rep1N) {
			cached = new combination::Rep1N(rw(Car(Cdr(n))));
		} else if(tag==STR_RPL) {
			cached = NULL;
		} else if(tag==STR_STR) {
			ast_node_t x = Cdr(n);
			cached = new token::Str(Value(Car(x)), Value(Car(Cdr(x))));
		} else if(tag==STR_BOW) {
			ast_node_t x = Cdr(n);
			cached = new token::Bow(Value(Car(x)), !!Cdr(x));
		} else if(tag==STR_AddToBag) {
			cached = NULL;
		} else if(tag==STR_Prefix) {
			ast_node_t x = Cdr(n);
			ast_node_t nt = Car(Cdr(x));
			cached = new combination::Prefix(rw(Car(x)), Value(Car(Cdr(nt))));
		} else if(tag==STR_Postfix) {
			ast_node_t x = Cdr(n);
			ast_node_t nt = Car(Cdr(x));
			cached = new combination::Postfix(rw(Car(x)), Value(Car(Cdr(nt))));
		} else if(tag==STR_TransientRule) {
			ast_node_t x = Cdr(n);
			cached = new rule::Transient(Value(Car(x)), from_ast(Car(Cdr(x)), g), g);
		} else if(tag==STR_OperatorRule) {
			ast_node_t x = Cdr(n);
			cached = new rule::Operator(Value(Car(x)), from_ast(Car(Cdr(x)), g), g);
		} else if(tag==STR_Space) {
			/*cached = new token::Nt(STR_Space);*/
			cached = NULL;
		} else if(tag==STR_NewLine) {
			/*cached = new token::Nt(STR_NewLine);*/
			cached = NULL;
		} else if(tag==STR_Indent) {
			/*cached = new token::Nt(STR_Indent);*/
			cached = NULL;
		} else if(tag==STR_Dedent) {
			/*cached = new token::Nt(STR_Dedent);*/
			cached = NULL;
		}
		/*std::cout << "adding item ";*/
		/*visitors::debugger d;*/
		/*cached->accept(&d);*/
		/*std::cout << " in cache for node " << ast_serialize_to_string(n) << std::endl;*/
		registry[n] = cached;
		/*std::cout << "now registry[" << ast_serialize_to_string(n) << "] = ";*/
		/*registry[n]->accept(&d);*/
		/*std::cout << std::endl;*/
		return cached;
	}


	ext::hash_map<const char*, trie_t> token::Bow::all;

	iterator iterator::create(const base*item) {
		visitors::iterator_factory f;
		return iterator(f(item));
	}

namespace combination {
	std::pair<ast_node_t, unsigned int> RawSeq::recognize(const char* source, unsigned int offset, unsigned int size) const
	{
		/*visitors::debugger d;*/
		rule::internal::append append;
		RawSeq::const_iterator i, j;
		std::pair<ast_node_t, unsigned int> ret(NULL, offset);
		/*std::cout << "matching rawseq ? " << std::string(source+offset, source+offset+20) << std::endl;*/
		for(i=begin(), j=end();ret.second<size&&i!=j&&ret.second<=size;++i) {
			/*(*i)->accept(&d);*/
			std::pair<ast_node_t, unsigned int> tmp = (*i)->recognize(source, ret.second, size);
			if(tmp.first) {
				/*std::cout << " matched" << std::endl;*/
				ret.first = append(tmp.first, ret.first);
				ret.second = tmp.second;
			} else {
				/*std::cout << " failed" << std::endl;*/
				return std::pair<ast_node_t, unsigned int>(NULL, offset);
			}
		}
		if(i!=j) {
			/*std::cout << "failed at end of text" << std::endl;*/
			return std::pair<ast_node_t, unsigned int>(NULL, offset);
		}
		/*std::cout << "OK ! new offset = " << ret.second << std::endl;*/
		return ret;
	}
}
}

namespace rule {
	void base::init(const char* name, item::base* _, Grammar*g)
	{
		tag_ = name;
		if(!_) { std::cout << "NULL rmember !" << std::endl; throw "COIN!"; return; }
		visitors::rmember_rewriter rw(g, this);
		visitors::debugger debug;
		/*std::cout << " adding rmember "; _->accept(&debug); std::cout << std::endl;*/
		item::base* rmb = rw(_);
		if(rmb) {
			/*std::cout << "  => as "; _->accept(&debug); std::cout << std::endl;*/
			/*push_back(rmb);*/
			insert(rmb);
		/*} else {*/
			/*std::cout << "  => skipped rmb in " << name << std::endl;*/
			/*push_back(new item::token::Epsilon());*/
			/*insert(new item::token::Epsilon());*/
		}
	}
}


Grammar::Grammar(ast_node_t rules) {
	std::cout << "DEBUG GRAMMAR " << ast_serialize_to_string(rules) << std::endl;
	std::cout << "pouet" << std::endl;
	while(rules) {
		ast_node_t rule = Car(rules);
		if(regstr(Value(Car(rule)))!=STR_Comment) {
			const char* tag = Value(Car(Cdr(rule)));
			add_rule(tag, dynamic_cast<rule::base*>(item::base::from_ast(rule, this)));
		}
		rules = Cdr(rules);
	}
	/* initialize whitespace-skipping subsystem */
	rule::base* WS = (*this)["_whitespace"];
	ws = NULL;
	if(WS) {
		/*std::cout << "have user-defined whitespaces" << std::endl;*/
		item::iterator WSi = item::iterator::create(WS);
		item::iterator expri = item::iterator::create(*WSi);
		const item::token::Re* check_re = dynamic_cast<const item::token::Re*>(*expri);
		if(check_re) {
			/*std::cout << "     user-defined whitespaces /" << check_re->pattern() << '/' << std::endl;*/
			ws = new ws_re(check_re->pattern());
		} else {
			const item::token::T* check_t = dynamic_cast<const item::token::T*>(*expri);
			if(check_t) {
				/*std::cout << "     user-defined whitespaces \"" << check_re->pattern() << '"' << std::endl;*/
				ws = new ws_str(check_t->str());
			}
		}
	}
	/* default to basic whitespace definition */
	if(!ws) {
		ws = new ws_str(" \t\n\r");
	}
}

Grammar::~Grammar() {
	delete ws;
	/*iterator i=begin(), j=end();*/
	/*for(;i!=j;++i) {*/
		/*delete i->second;*/
	/*}*/
}


}/* namespace grammar */


namespace io {
}

