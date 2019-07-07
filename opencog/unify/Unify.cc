/**
 * Unify.cc
 *
 * Utilities for unifying atoms.
 *
 * Copyright (C) 2016 OpenCog Foundation
 * All Rights Reserved
 * Author: Nil Geisweiller
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "Unify.h"

#include <opencog/util/algorithm.h>
#include <opencog/atoms/base/Atom.h>
#include <opencog/atoms/base/Node.h>
#include <opencog/atoms/core/Context.h>
#include <opencog/atoms/core/FindUtils.h>
// #include <opencog/atoms/core/Quotation.h>
#include <opencog/atoms/core/TypeUtils.h>
// #include <opencog/atoms/core/Variables.h>
#include <opencog/atoms/core/RewriteLink.h>
#include <opencog/atoms/pattern/PatternUtils.h>
#include <opencog/atomspace/AtomSpace.h>

namespace opencog {

const Unify::Partitions Unify::empty_partitions({});

const Unify::Partitions Unify::empty_partition_singleton({{}});

Unify::CHandle::CHandle(const Handle& h, const Context& c)
	: handle(h), context(c) {}

bool Unify::CHandle::is_variable() const
{
	return handle->get_type() == VARIABLE_NODE;
}

bool Unify::CHandle::is_free_variable() const
{
	return context.is_free_variable(handle);
}

HandleSet Unify::CHandle::get_free_variables() const
{
	HandleSet free_vars =
		opencog::get_free_variables(handle, context.quotation);
	return set_difference(free_vars, context.shadow);
}

Context::VariablesStack::const_iterator
Unify::CHandle::find_variables(const Handle& h) const
{
	return std::find_if(context.scope_variables.cbegin(),
	                    context.scope_variables.cend(),
	                    [&](const Variables& variables) {
		                    return variables.is_in_varset(h);
	                    });
}

bool Unify::CHandle::is_consumable() const
{
	return context.quotation.consumable(handle->get_type());
}

bool Unify::CHandle::is_quoted() const
{
	return context.quotation.is_quoted();
}

bool Unify::CHandle::is_unquoted() const
{
	return context.quotation.is_unquoted();
}

void Unify::CHandle::update()
{
	bool isc = is_consumable();
	context.update(handle);
	if (isc)
		handle = handle->getOutgoingAtom(0);
}

bool Unify::CHandle::is_node_satisfiable(const CHandle& other) const
{
	// If both are variable check whether they could be alpha
	// equivalent, otherwise merely check for equality
	if (is_variable() and other.is_variable())	{
		// Make sure scope variable declarations are stored
		OC_ASSERT(context.store_scope_variables,
		          "You must store the scope variable declarations "
		          "in order to use this method");

		// Search variable declarations associated to the variables
		Context::VariablesStack::const_iterator it = find_variables(handle),
			other_it = other.find_variables(other.handle);
		OC_ASSERT(it != context.scope_variables.cend(),
		          "Contradicts the assumption that this->handle is not free");
		OC_ASSERT(other_it != other.context.scope_variables.cend(),
		          "Contradicts the assumption that other.handle is not free");

		// Check that both variable declarations occured at the same level
		if (std::distance(context.scope_variables.cbegin(), it)
		    != std::distance(other.context.scope_variables.cbegin(), other_it))
			return false;

		// Check that the other variable is alpha convertible
		return it->is_alpha_convertible(handle, other.handle, *other_it, true);
	} else {
		return content_eq(handle, other.handle);
	}
}

bool Unify::CHandle::operator==(const CHandle& ch) const
{
	return content_eq(handle, ch.handle) and (context == ch.context);
}

bool Unify::CHandle::operator<(const CHandle& ch) const
{
	return (handle < ch.handle) or
		(handle == ch.handle and context < ch.context);
}

Unify::CHandle::operator bool() const
{
	return (bool)handle;
}

Unify::SolutionSet::SolutionSet(bool s)
	: Partitions(s ? empty_partition_singleton : empty_partitions) {}

Unify::SolutionSet::SolutionSet(const Unify::Partitions& p)
	: Partitions(p) {}

bool Unify::SolutionSet::is_satisfiable() const
{
	return not empty();
}

Unify::Unify(const Handle& lhs, const Handle& rhs,
             const Handle& lhs_vardecl, const Handle& rhs_vardecl)
{
	// Set terms to unify
	_lhs = lhs;
	_rhs = rhs;

	// Set _variables
	set_variables(lhs, rhs, lhs_vardecl, rhs_vardecl);
}

Unify::Unify(const Handle& lhs, const Handle& rhs,
             const Variables& lhs_vars, const Variables& rhs_vars)
{
	// Set terms to unify
	_lhs = lhs;
	_rhs = rhs;

	// Set _variables
	_variables = merge_variables(lhs_vars, rhs_vars);
}

Unify::TypedSubstitutions Unify::typed_substitutions(const SolutionSet& sol,
                                                     const Handle& pre) const
{
	OC_ASSERT(sol.is_satisfiable());

	TypedSubstitutions result;
	for (const Partition& partition : sol)
		result.insert(typed_substitution(partition, pre));
	return result;
}

Unify::TypedSubstitution Unify::typed_substitution(const Partition& partition,
                                                   const Handle& pre) const
{
	// Associate the least abstract element to each variable of each
	// block.
	HandleCHandleMap var2cval;
	for (const TypedBlock& block : partition) {
		CHandle least_abstract = find_least_abstract(block, pre);

		// Build variable mapping
		for (const CHandle& ch : block.first)
			if (ch.is_free_variable())
				var2cval.insert({ch.handle, least_abstract});
	}

	// Calculate its closure
	var2cval = substitution_closure(var2cval);

	// Remove ill quotations
	for (auto& vcv : var2cval) {
		Handle consumed =
			RewriteLink::consume_quotations(_variables, vcv.second.handle,
			                                vcv.second.context.quotation, false);
		vcv.second = CHandle(consumed, vcv.second.context);
	}

	// Calculate its variable declaration
	Handle vardecl = substitution_vardecl(var2cval);

	// Return the typed substitution
	return {var2cval, vardecl};
}

void Unify::set_variables(const Handle& lhs, const Handle& rhs,
                          const Handle& lhs_vardecl, const Handle& rhs_vardecl)
{
	// Merge the 2 type declarations
	Variables lv = gen_varlist(lhs, lhs_vardecl)->get_variables();
	Variables rv = gen_varlist(rhs, rhs_vardecl)->get_variables();
	_variables = merge_variables(lv, rv);
}

Unify::CHandle Unify::find_least_abstract(const TypedBlock& block,
                                          const Handle& pre) const
{
	// Get the least abstract element of the block
	static Handle top(Handle(createNode(VARIABLE_NODE, "__dummy_top__")));
	CHandle least_abstract(top);
	for (const CHandle& ch : block.first)
		if (inherit(ch, least_abstract))
			least_abstract = ch;

	// In case of ties pick up the one in pre (pre stands for
	// precedence)
	for (const CHandle& ch : block.first)
		if (inherit(ch, least_abstract)
		    and
		    (not ch.is_free_variable()
		     or is_unquoted_unscoped_in_tree(pre, ch.handle)))
			least_abstract = ch;

	OC_ASSERT(least_abstract.handle != top,
	          "Finding the least abstract atom in the block has failed. "
	          "It is probably a bug.");

	return least_abstract;
}

Unify::HandleCHandleMap Unify::substitution_closure(const HandleCHandleMap& var2cval) const
{
	// Strip var2cval from its contexts
	HandleMap var2val = strip_context(var2cval);

	// Subtitute every value that have variables by other values
	// associated to these variables.
	HandleCHandleMap result(var2cval);
	for (auto& el : result) {
		VariableListPtr varlist = gen_varlist(el.second);
		const Variables& variables = varlist->get_variables();
		HandleSeq values = variables.make_sequence(var2val);
		el.second.handle = variables.substitute_nocheck(el.second.handle, values);
	}

	// If we have reached a fixed point then return substitution,
	// otherwise re-iterate
	return hchm_content_eq(result, var2cval) ?
		result : substitution_closure(result);
}

Handle Unify::substitution_vardecl(const HandleCHandleMap& var2val) const
{
	// Build the type declaration for this substitution. For now, the
	// type is merely lhs_vardecl and rhs_vardecl merged together,
	// then all variables assigned for substitution other than
	// themselves are removed. To do well it should be taking into
	// account the possibly more restrictive types found during
	// unification (i.e. the block types).

	Variables ts_variables = _variables;

	for (const auto& el : var2val)
		// Make sure it is not a self substitution
		if (el.first != el.second.handle)
			ts_variables.erase(el.first);

	return ts_variables.get_vardecl();
}

bool Unify::is_pm_connector(const Handle& h)
{
	return is_pm_connector(h->get_type());
}

bool Unify::is_pm_connector(Type t)
{
	return t == AND_LINK or t == OR_LINK or t == NOT_LINK;
}

Handle Unify::substitute(BindLinkPtr bl, const TypedSubstitution& ts,
                         const AtomSpace* queried_as)
{
	// TODO: make sure that ts.second contains the declaration of all
	// variables
	return substitute(bl, strip_context(ts.first), ts.second, queried_as);
}

Handle Unify::substitute(BindLinkPtr bl, const HandleMap& var2val,
                         Handle vardecl, const AtomSpace* queried_as)
{
	// Perform substitution over the existing variable declaration, if
	// no new alternative is provided.
	if (not vardecl) {
		// If the bind link has no variable declaration either then
		// infer one
		Handle old_vardecl = bl->get_vardecl() ? bl->get_vardecl()
			: gen_vardecl(bl->get_body());
		// Substitute the variables in the old vardecl to obtain the
		// new one.
		vardecl = substitute_vardecl(old_vardecl, var2val);
	}

	const Variables variables = bl->get_variables();

	// Turn the map into a vector of new variable names/values
	HandleSeq values = variables.make_sequence(var2val);

	// Substituted BindLink outgoings
	HandleSeq hs;

	// Perform substitution over the pattern term, then remove
	// constant clauses
	Handle clauses = variables.substitute_nocheck(bl->get_body(), values);
	clauses = RewriteLink::consume_quotations(vardecl, clauses, true);
	if (queried_as)
		clauses = remove_constant_clauses(vardecl, clauses, queried_as);
	hs.push_back(clauses);

	// Perform substitution over the rewrite term
	Handle rewrite = variables.substitute_nocheck(bl->get_implicand(), values);
	rewrite = RewriteLink::consume_quotations(vardecl, rewrite, false);
	hs.push_back(rewrite);

	// Filter vardecl
	vardecl = filter_vardecl(vardecl, hs);

	// Insert vardecl in hs if defined
	if (vardecl)
		hs.insert(hs.begin(), vardecl);

	// Create the substituted BindLink
	return createLink(hs, bl->get_type());
}

Handle Unify::substitute_vardecl(const Handle& vardecl,
                                 const HandleMap& var2val)
{
	if (not vardecl)
		return Handle::UNDEFINED;

	Type t = vardecl->get_type();

	// Base cases

	if (t == VARIABLE_NODE) {
		auto it = var2val.find(vardecl);
		// Only substitute if the variable is substituted by another variable
		if (it != var2val.end() and it->second->get_type() == VARIABLE_NODE)
			return it->second;
		return Handle::UNDEFINED;
	}

	// Recursive cases

	HandleSeq oset;

	if (t == VARIABLE_LIST) {
		for (const Handle& h : vardecl->getOutgoingSet()) {
			Handle nh = substitute_vardecl(h, var2val);
			if (nh)
				oset.push_back(nh);
		}
		if (oset.empty())
			return Handle::UNDEFINED;
	}
	else if (t == TYPED_VARIABLE_LINK) {
		Handle new_var = substitute_vardecl(vardecl->getOutgoingAtom(0),
		                                    var2val);
		if (new_var) {
			oset.push_back(new_var);
			oset.push_back(vardecl->getOutgoingAtom(1));
		} else return Handle::UNDEFINED;
	}
	else {
		OC_ASSERT(false, "Not implemented");
	}
	return createLink(oset, t);
}


static bool not_in_atomspace(const Handle& handle, const AtomSpace* atomspace)
{
	return nullptr != atomspace
	   and nullptr == atomspace->get_atom(handle);
}

// Is a clause constant, relative to some atomspace?
// Why would it matter whether or not it is in some atomspace?
static bool not_constant(const HandleSet& vars,
                         const Handle& clause,
                         const AtomSpace* as)
{
	return not_in_atomspace(clause, as) or not is_constant(vars, clause);
}

// TODO: for now it is assumed clauses are connected by an AndLink
// only. To fix that one needs to generalize
// PatternLink::unbundle_clauses to make it usable in that code too.
//
// TODO: maybe replace Handle vardecl by Variables variables.
Handle Unify::remove_constant_clauses(const Handle& vardecl,
                                      const Handle& clauses,
                                      const AtomSpace* as)
{
	VariableListPtr vl = createVariableList(vardecl);
	HandleSet vars = vl->get_variables().varset;

	// Remove constant clauses
	Type t = clauses->get_type();
	HandleSeq hs;
	if (t == AND_LINK) {
		for (const Handle& clause : clauses->getOutgoingSet()) {
			if (not_constant(vars, clause, as)) {
				hs.push_back(clause);
			}
		}
	} else if (not_constant(vars, clauses, as)) {
		return clauses;
	}
	return createLink(hs, AND_LINK);
}

Unify::SolutionSet Unify::operator()()
{
	// If the declaration is ill typed, there is no solution
	if (not _variables.is_well_typed())
		return SolutionSet();

	// It is well typed, perform the unification
	return unify(_lhs, _rhs);
}

Unify::SolutionSet Unify::unify(const CHandle& lhs, const CHandle& rhs) const
{
	return unify(lhs.handle, rhs.handle, lhs.context, rhs.context);
}

Unify::SolutionSet Unify::unify(const Handle& lh, const Handle& rh,
                                Context lc, Context rc) const
{
	Type lt(lh->get_type());
	Type rt(rh->get_type());

	///////////////////
	// Base cases    //
	///////////////////

	// Make sure both handles are defined
	if (not lh or not rh)
		return SolutionSet();

	CHandle lch(lh, lc);
	CHandle rch(rh, rc);

	bool lq = lc.quotation.consumable(lt);
	bool rq = rc.quotation.consumable(rt);

	// If one is a node
	if (lh->is_node() or rh->is_node()) {
		// If one is a free variable and they are different, then
		// unifies.
		if (lch.is_free_variable() or rch.is_free_variable()) {
			if (lch == rch) {
				// Do not construct a solution like {X}->X to not
				// overload the solution set.
				//
				// Since the context is taken into account they have
				// the same context, thus if one of them is free, the
				// other is free as well, therefore they are
				// satisfiable.
				return SolutionSet(true);
			} else {
				return mkvarsol(lch, rch);
			}
		}
		else if (lt == GLOB_NODE or rt == GLOB_NODE)
		{
			return mkvarsol(lch, rch);
		}
		else if (!lq and !rq)
			return SolutionSet(lch.is_node_satisfiable(rch));
	}

	////////////////////////
	// Recursive cases    //
	////////////////////////

    // Consume quotations
	if (lq and rq) {
		lc.quotation.update(lt);
		rc.quotation.update(rt);
		return unify(lh->getOutgoingAtom(0), rh->getOutgoingAtom(0), lc, rc);
	}
	if (lq) {
		lc.quotation.update(lt);
		return unify(lh->getOutgoingAtom(0), rh, lc, rc);
	}
	if (rq) {
		rc.quotation.update(rt);
		return unify(lh, rh->getOutgoingAtom(0), lc, rc);
	}

	// Update contexts
	lc.update(lh);
	rc.update(rh);

	// At least one of them is a link, check if they have the same
	// type (e.i. do they match so far)
	if (lt != rt)
		return SolutionSet();

	// At this point they are both links of the same type, check that
	// they have the same arity
	Arity lh_arity(lh->get_arity());
	Arity rh_arity(rh->get_arity());
	if (lh_arity != rh_arity) {
		if (contain_glob(lh) or contain_glob(rh)) {
			if (is_unordered(rh))
				return unordered_glob_unify(lh->getOutgoingSet(), rh->getOutgoingSet(), lc, rc);
			else
				OC_ASSERT(false, "not implemented yet");
		}
		return SolutionSet();
	}

	if (is_unordered(rh))
		return unordered_unify(lh->getOutgoingSet(), rh->getOutgoingSet(), lc, rc);
	else
		return ordered_unify(lh->getOutgoingSet(), rh->getOutgoingSet(), lc, rc);
}

Unify::SolutionSet Unify::unordered_unify(const HandleSeq& lhs,
                                          const HandleSeq& rhs,
                                          Context lc, Context rc) const
{
	Arity lhs_arity(lhs.size());
	Arity rhs_arity(rhs.size());
	OC_ASSERT(lhs_arity == rhs_arity);

	// Base case
	if (lhs_arity == 0)
		return SolutionSet(true);

	// Recursive case
	SolutionSet sol;
	for (Arity i = 0; i < lhs_arity; ++i) {
		auto head_sol = unify(lhs[i], rhs[0], lc, rc);
		if (head_sol.is_satisfiable()) {
			HandleSeq lhs_tail(cp_erase(lhs, i));
			HandleSeq rhs_tail(cp_erase(rhs, 0));
			auto tail_sol = unordered_unify(lhs_tail, rhs_tail, lc, rc);
			SolutionSet perm_sol = join(head_sol, tail_sol);
			// Union merge satisfiable permutations
			sol.insert(perm_sol.begin(), perm_sol.end());
		}
	}
	return sol;
}

Unify::SolutionSet Unify::ordered_unify(const HandleSeq& lhs,
                                        const HandleSeq& rhs,
                                        Context lc, Context rc) const
{
	Arity lhs_arity(lhs.size());
	Arity rhs_arity(rhs.size());
	OC_ASSERT(lhs_arity == rhs_arity);

	SolutionSet sol(true);
	for (Arity i = 0; i < lhs_arity; ++i) {
		auto rs = unify(lhs[i], rhs[i], lc, rc);
		sol = join(sol, rs);
		if (not sol.is_satisfiable())     // Stop if unification has failed
			break;
	}
	return sol;
}

Unify::SolutionSet
Unify::unordered_glob_unify(const HandleSeq &lhs, const HandleSeq &rhs,
		Context lhs_context, Context rhs_context) const
{
	SolutionSet sol;
	GBlock l_glob_set, l_term_set,
			r_glob_set, r_term_set;

	parse_type(lhs, l_glob_set, l_term_set);
	parse_type(rhs, r_glob_set, r_term_set);

	SolutionsPairs term_solutions = unify_terms(l_term_set, r_term_set);
	// Add empty term_solutions as one possibility.
	std::pair<Indices, Indices> def_index({});
	SolutionSet def_sol({});
	term_solutions.push_back({def_sol, def_index});

	sol = unify_globs(term_solutions, l_glob_set, l_term_set, r_glob_set, r_term_set,
	                  lhs_context, rhs_context);

	return sol;
}

Unify::SolutionSet
Unify::unordered_glob_sub_unify(const GPart part, Context lhscontext,
                                Context rhsContext) const
{
	return Unify::SolutionSet();
}

Unify::SolutionSet Unify::unordered_glob_partial_unify(const HandleSeq &lhs, const HandleSeq &rhs, Context lhs_context,
                                                       Context rhs_context) const
{
	Arity lhs_arity(lhs.size());
	Arity rhs_arity(rhs.size());

	SolutionSet sol;
	for (Arity i = 0; i < lhs_arity; ++i) {
		if(!rhs_arity) return sol;
		auto head_sol = unify(lhs[i], rhs[0], lhs_context, rhs_context);
		if (head_sol.is_satisfiable()) {
			HandleSeq lhs_tail;
			HandleSeq rhs_tail;
			if (lhs[i]->get_type() == GLOB_NODE) {
				SolutionSet tail_sol;
				lhs_tail = HandleSeq(lhs);
				rhs_tail = HandleSeq(cp_erase(rhs, 0));
				do {
					tail_sol = unordered_glob_partial_unify(lhs_tail, rhs_tail, lhs_context, rhs_context);
					if (tail_sol.is_satisfiable()) {
						// since join is not implemented for GLOB_NODE I am Using insert for now
						// SolutionSet perm_sol = join(head_sol, tail_sol);
						SolutionSet perm_sol;
						perm_sol.insert(head_sol.begin(), head_sol.end());
						perm_sol.insert(tail_sol.begin(), tail_sol.end());

						// Union merge satisfiable permutations
						sol.insert(perm_sol.begin(), perm_sol.end());
					} else {
						lhs_tail = HandleSeq(cp_erase(lhs, i));
						if (lhs_tail.size() == rhs_tail.size() && rhs_tail.size() == 0) {
							sol.insert(head_sol.begin(), head_sol.end());
							break;
						}
						rhs_tail = HandleSeq(rhs);
						head_sol.clear();
					}
				} while (!tail_sol.is_satisfiable());
			}
		}
	}
	return sol;
}

void Unify::parse_type(const HandleSeq &seq, Unify::GBlock &glob_seq, Unify::GBlock &term_seq) const
{
	for (const auto handle : seq) {
		Type type = handle->get_type();
		// TODO handle VariableNode
		if (type == GLOB_NODE) {
			if (_variables.is_in_varset(handle)) {
				auto ts = (*_variables._simple_typemap.find(handle)).second;
				// TODO: ts is TypeSet figure out when and why a variable would
				//  contain multiple types. and edit the /*ts.begin()/ accordingly.
				insert_type(glob_seq, handle, *ts.begin(), true);
			}
			else {
				insert_type(glob_seq, handle, NOTYPE);
			}
		}
		else {
			insert_type(term_seq, handle, handle->get_type());
		}
	}
}

void Unify::insert_type(Unify::GBlock &vector, const Handle &handle, Type type, bool offset) const
{
	if (vector.empty()) {
		if (offset) {vector.push_back({});}
		HandleSeq seq = {handle};
		vector.push_back(seq);
		return;
	}

	if (type == NOTYPE) {
		vector[0].push_back(handle);
		return;
	}

	for (HandleSeq &v : vector) {
		if (type == (*v.begin())->get_type()) {
			v.push_back(handle);
			return;
		}
	}

	HandleSeq seq = {handle};
	vector.push_back(seq);
}

Unify::SolutionsPairs Unify::unify_terms(const Unify::GBlock &l_terms,
                                         const Unify::GBlock &r_terms) const
{
	if (l_terms.empty() or r_terms.empty()) return {};

	SolutionsPairs sol_map;
	const HandleSeq r_t = r_terms[0];

	for (Arity l = 0; l < l_terms.size(); l++) {
		const HandleSeq l_t = l_terms[l];
		if (l_t[0]->get_type() == r_t[0]->get_type()) {
			auto sub_sol_map = unify_term_sub(l_t, r_t);
			for (auto pair : sub_sol_map) {
				auto ss = pair.first;
				auto index = pair.second;
				Indices l_i = {l, index.first};
				Indices r_i = {0, index.second};
				std::pair<Indices, Indices> index_pair = {l_i, r_i};
				sol_map.push_back({ss, index_pair});
			}

			GBlock _l_terms(l_terms);
			GBlock _r_terms(r_terms);
			_l_terms.erase(_l_terms.begin() + l);
			_r_terms.erase(_r_terms.begin() + 0);

			auto _sol_map = unify_terms(_l_terms, _r_terms);
			for (auto pair : _sol_map) {
				auto ss = pair.first;
				auto indices = pair.second;
				Arity offset = indices.first[0] >= l ? indices.first[0] + 1 : indices.first[0];
				Indices l_i = {offset, indices.first[1]};
				Indices r_i = {indices.second[0] + 1, indices.second[1]};
				std::pair<Indices, Indices> indices_pair = {l_i, r_i};
				sol_map.push_back({ss, indices_pair});
			}
			return sol_map;
		}
	}

	GBlock _r_terms(r_terms);
	_r_terms.erase(_r_terms.begin() + 0);

	auto _sol_map = unify_terms(l_terms, _r_terms);
	for (auto pair : _sol_map) {
		auto ss = pair.first;
		auto indices = pair.second;
		Indices l_i = {indices.first[0], indices.first[1]};
		Indices r_i = {indices.second[0] + 1, indices.second[1]};
		std::pair<Indices, Indices> indices_pair = {l_i, r_i};
		sol_map.push_back({ss, indices_pair});
	}
	return sol_map;
}

Unify::SolutionPairs Unify::unify_term_sub(const HandleSeq &l_term, const HandleSeq &r_term) const
{
	if (l_term.empty() or r_term.empty()) return {};

	SolutionPairs sol_map;
	const Handle r_t = r_term[0];

	for (Arity l = 0; l < l_term.size(); l++) {
		const Handle l_t = l_term[l];
		auto ss = unify(l_t, r_t);

		if (ss.is_satisfiable()) {
			PairIndice idx = {l, 0};
			sol_map.push_back({ss, idx});

			HandleSeq _l_term(l_term);
			HandleSeq _r_term(r_term);
			_l_term.erase(_l_term.begin() + l);

			auto _sol_map = unify_term_sub(_l_term, _r_term);

			for (auto pair : _sol_map) {
				auto ss = pair.first;
				auto index = pair.second;
				Arity l_i = index.first >= l ? index.first + 1 : index.first;
				Arity r_i = index.second;
				std::pair<Arity, Arity> idx_pair = {l_i, r_i};
				sol_map.push_back({ss, idx_pair});
			}
			return sol_map;
		}
	}

	HandleSeq _r_term(r_term);
	_r_term.erase(_r_term.begin() + 0);

	auto _sol_map = unify_term_sub(l_term, _r_term);
	for (auto pair : _sol_map) {
		auto ss = pair.first;
		auto index = pair.second;
		Arity l_i = index.first;
		Arity r_i = index.second + 1;
		std::pair<Arity, Arity> idx_pair = {l_i, r_i};
		sol_map.push_back({ss, idx_pair});
	}
	return sol_map;
}

Unify::SolutionSet
Unify::unify_globs(const Unify::SolutionsPairs &term_solutions, Unify::GBlock &l_glob,
                   Unify::GBlock &l_term, Unify::GBlock &r_glob, Unify::GBlock &r_term,
                   Context &lhs_context, Context &rhs_context) const
{
	SolutionSet sol;
	for (auto pair : term_solutions) {
		auto ss = pair.first;
		auto rm_indices = pair.second;

		GBlock _l_term_set(l_term);
		GBlock _r_term_set(r_term);
		if (!rm_indices.first.empty()) {
			auto left_rm_terms = _l_term_set.begin() + rm_indices.first[0];
			left_rm_terms->erase(left_rm_terms->begin() + rm_indices.first[1]);
			if (left_rm_terms->empty()) _l_term_set.erase(left_rm_terms);
			auto right_rm_terms = _r_term_set.begin() + rm_indices.second[0];
			right_rm_terms->erase(right_rm_terms->begin() + rm_indices.second[1]);
			if (right_rm_terms->empty()) _r_term_set.erase(right_rm_terms);
		}

		if ((_l_term_set.empty() and (!r_glob.empty())) or
		    ((!_l_term_set.empty()) and r_glob.empty()) or
		    (_r_term_set.empty() and (!l_glob.empty())) or
		    ((!_r_term_set.empty()) and l_glob.empty()))
			continue;

		GSolution _l_gSolution = build_solution(l_glob, _r_term_set);
		GSolution _r_gSolution = build_solution(r_glob, _l_term_set);
		for (const auto l_gpart : _l_gSolution) {
			SolutionSet p_sol;
			for (const auto r_gpart : _r_gSolution) {
				GPart part(l_gpart);
				part.insert(r_gpart.begin(), r_gpart.end());
				auto _ss = unordered_glob_sub_unify(part, lhs_context, rhs_context);
				// TODO join ss, _ss
				_ss.insert(ss.begin(), ss.end());
				p_sol.insert(_ss.begin(), _ss.end());
			}
			sol.insert(p_sol.begin(), p_sol.end());
		}
	}
	return sol;
}

Unify::GSolution Unify::build_solution(Unify::GBlock &glob_seq, Unify::GBlock &term_seq) const
{
	// Minimum number of type of terms glob_set can hold.
	Arity min_glob_arity = glob_seq.size();

	// Number of term types to hold.
	Arity term_arity = term_seq.size();

	if (term_arity == 0 or min_glob_arity == 0) return Unify::GSolution({{}});

	// Maximum number of type of terms glob_set can hold.
	// Type undeclared GlobNodes can hold multiple typed terms, each a single type.
	Arity max_glob_arity = glob_seq.size() + glob_seq[0].size() - 1;

	if (not(min_glob_arity <= term_arity) and
	    not(term_arity <= max_glob_arity)) {
		return Unify::GSolution();
	}

	return combin_gblock(glob_seq, term_seq);
}

Unify::GSolution Unify::combin_gblock(Unify::GBlock &glob_set, Unify::GBlock &term_set) const
{
	GSolution sol;

	HandleSeq untyped_globs = glob_set[0];
	HandleSeqSeq typed_globs(glob_set.begin()+1, glob_set.end());

	Arity terms_size = term_set.size();
	Arity glob_size = glob_set.size();
	Arity untyped_globs_size = untyped_globs.size();

	// i:e {[G][][]----[T1][T2][T3][T4]} [G] can be splited to at most four groups.
	Arity max_num_splits = untyped_globs_size > terms_size ? terms_size : untyped_globs_size;
	// i:e {[G][][]----[T1][T2][T3][T4]} we need [G] to be splited to at least two.
	Arity min_num_splits = (terms_size - glob_size) + 1;

	std::set<HandleSeqSeq> untyped_globs_set;
	for (Arity s = min_num_splits; s <= max_num_splits; ++s) {
		std::set<HandleSeqSeq> ss = split_globs(untyped_globs, s);
		untyped_globs_set.insert(ss.begin(), ss.end());
	}

	std::set<HandleSeqSeq> glob_sets;
	for (auto g_set : untyped_globs_set) {
		std::set<HandleSeqSeq> ss = merge_globs(g_set, typed_globs, terms_size);
		glob_sets.insert(ss.begin(), ss.end());
	}

	sol = perm(glob_sets, term_set);
	return sol;
}

std::set<HandleSeqSeq> Unify::split_globs(HandleSeq &globs, Arity s) const
{
	std::set<HandleSeqSeq> result;

	if (s == 1) result = {{globs}};

	else if (s == 2) {
		// TODO Handle redundant selects
		for (Arity r = 1; r <= globs.size() - 1; r++) {
			std::set<HandleSeqSeq> splited = select_subset(globs, r);
			result.insert(splited.begin(), splited.end());
		}
	}

	else {
		std::set<HandleSeqSeq> shss = split_globs(globs, s - 1);
		for (HandleSeqSeq hss : shss) {
			// assuming the zeroes item will be the longest
			std::set<HandleSeqSeq> _shss = split_globs(hss[0], 2);
			for (HandleSeqSeq _hss : _shss) {
				HandleSeqSeq tmp(_hss.begin(), _hss.end());
				tmp.insert(tmp.end(), hss.begin()+1, hss.end());
				result.insert(tmp);
			}
		}
	}

	return result;
}

std::set<HandleSeqSeq> Unify::select_subset(HandleSeq &unt_globs, Arity s) const
{
	std::set<HandleSeqSeq> total_sol;
	Indices ids(s);
	std::iota(ids.begin(), ids.end(), 0);

	while (ids[0] <= unt_globs.size() - s) {
		HandleSeqSeq sol={};
		HandleSeq left;
		HandleSeq right(unt_globs);
		for (int i=ids.size()-1; i>=0; i--) {
			left.push_back(right[ids[i]]);
			right.erase(right.begin()+ids[i]);
		}
		sol.push_back(left);
		sol.push_back(right);
		total_sol.insert(sol);
		increament(ids, unt_globs.size());
	}
	return total_sol;
}

void Unify::increament(Unify::Indices &ids, Arity s) const
{
	Arity li=0;
	for (Arity i=0; i<ids.size(); i++) {
		if (ids[i] < s - (ids.size() - i)) li=i;
	}
	ids[li] += 1;
	for (Arity i=li+1; i<ids.size(); i++) {
		ids[i] = ids[i-1] + 1;
	}
}

std::set<HandleSeqSeq> Unify::merge_globs(HandleSeqSeq &untyped_globs, HandleSeqSeq &typed_globs, Arity size) const
{
	std::set<HandleSeqSeq> sol;

	Arity num_merge = typed_globs.size() - (size - untyped_globs.size());

	if (num_merge == 0) {
		HandleSeqSeq _hss(untyped_globs.begin(), untyped_globs.end());
		_hss.insert(_hss.end(), typed_globs.begin(), typed_globs.end());
		sol.insert(_hss);
		return sol;
	}

	for (Arity i=0; i < untyped_globs.size(); i++) {
		for(Arity j=0; j < typed_globs.size(); j++) {
			HandleSeq head(untyped_globs[i]);
			auto top_hs = typed_globs[j];
			head.insert(head.end(), top_hs.begin(), top_hs.end());

			// TODO avoid redundancy
			HandleSeqSeq l_rem(untyped_globs);
			HandleSeqSeq r_rem(typed_globs);

			l_rem.erase(l_rem.begin()+i);
			r_rem.erase(r_rem.begin()+j);

			auto tail = merge_globs(l_rem, r_rem, size-1);

			for (auto _hss : tail) {
				HandleSeqSeq _tmp(_hss);
				_tmp.push_back(head);
				sol.insert(_tmp);
			}
		}
	}
	return sol;
}

Unify::GSolution Unify::perm(std::set<HandleSeqSeq> &globs, Unify::GBlock &term) const
{
	GSolution sol;
	for (const HandleSeqSeq& hss : globs) {
		auto ss = calc_partition(hss, term);
		sol.insert(ss.begin(), ss.end());
	}
	return sol;
}

Unify::GSolution Unify::calc_partition(const HandleSeqSeq &glob, Unify::GBlock &term) const
{
	GSolution sol;
	for (Arity i=0; i<glob.size(); i++) {
		GMap m = {glob[i], term[0]};

		HandleSeqSeq _glob(glob);
		HandleSeqSeq _term(term);
		_glob.erase(_glob.begin()+i);
		_term.erase(_term.begin()+0);

		auto tail = calc_partition(_glob, _term);
		auto ss = join(m, tail);
		sol.insert(ss.begin(), ss.end());
	}
	return sol;
}

Unify::GSolution Unify::join(Unify::GMap &gmap, Unify::GSolution &gsolution) const
{
	GSolution gsol;

	if (gsolution.empty()) {
		GPart pr = {gmap};
		gsol.insert(pr);
		return gsol;
	}

	for (const auto gp : gsolution) {
		GPart pr;
		pr.insert(gmap);
		pr.insert(gp.begin(), gp.end());
		gsol.insert(pr);
	}
	return gsol;
}

Unify::SolutionSet Unify::pairwise_unify(const std::set<CHandlePair>& pchs) const
{
	SolutionSet sol(true);
	for (const CHandlePair& pch : pchs) {
		auto rs = unify(pch.first, pch.second);
		sol = join(sol, rs);
		if (not sol.is_satisfiable())     // Stop if unification has failed
			return sol;
	}
	return sol;
}

Unify::SolutionSet Unify::comb_unify(const std::set<CHandle>& lhs,
                                     const std::set<CHandle>& rhs) const
{
	SolutionSet sol(true);
	for (const CHandle& lch : lhs) {
		for (const CHandle& rch : rhs) {
			auto rs = unify(lch, rch);
			sol = join(sol, rs);
			if (not sol.is_satisfiable())     // Stop if unification has failed
				return sol;
		}
	}
	return sol;
}

Unify::SolutionSet Unify::comb_unify(const std::set<CHandle>& chs) const
{
	SolutionSet sol(true);
	for (auto lit = chs.begin(); lit != chs.end(); ++lit) {
		for (auto rit = std::next(lit); rit != chs.end(); ++rit) {
			auto rs = unify(*lit, *rit);
			sol = join(sol, rs);
			if (not sol.is_satisfiable())     // Stop if unification has failed
				return sol;
		}
	}
	return sol;
}
	
bool Unify::is_unordered(const Handle& h) const
{
	return nameserver().isA(h->get_type(), UNORDERED_LINK);
}

HandleSeq Unify::cp_erase(const HandleSeq& hs, Arity i) const
{
	HandleSeq hs_cp(hs);
	hs_cp.erase(hs_cp.begin() + i);
	return hs_cp;
}

Unify::SolutionSet Unify::mkvarsol(CHandle lch, CHandle rch) const
{
	// Attempt to consume quotation to avoid putting quoted elements
	// in the block.
	if (lch.is_free_variable() and rch.is_consumable() and rch.is_quoted())
		rch.update();
	if (rch.is_free_variable() and lch.is_consumable() and lch.is_quoted())
		lch.update();

	CHandle inter = type_intersection(lch, rch);
	if (not inter)
		return SolutionSet();
	else {
		Block pblock{lch, rch};
		Partitions par{{{pblock, inter}}};
		return SolutionSet(par);
	}
}

Unify::SolutionSet Unify::join(const SolutionSet& lhs,
                               const SolutionSet& rhs) const
{
	// No need to join if one of them is non satisfiable
	if (not lhs.is_satisfiable() or not rhs.is_satisfiable())
		return SolutionSet();

	// By now both are satisfiable, thus non empty, join them
	SolutionSet result;
	for (const Partition& rp : rhs) {
		SolutionSet sol(join(lhs, rp));
		result.insert(sol.begin(), sol.end());
	}

	return result;
}

Unify::SolutionSet Unify::join(const SolutionSet& lhs, const Partition& rhs) const
{
	// Base cases
	if (rhs.empty())
		return lhs;

	// Recursive case (a loop actually)
	SolutionSet result;
	for (const auto& par : lhs) {
		SolutionSet jps = join(par, rhs);
		result.insert(jps.begin(), jps.end());
	}
	return result;
}

Unify::SolutionSet Unify::join(const Partition& lhs, const Partition& rhs) const
{
	// Don't bother joining if lhs is empty (saves a bit of computation)
	if (lhs.empty())
		return SolutionSet({rhs});

	// Join
	SolutionSet result({lhs});
	for (const TypedBlock& rhs_block : rhs) {
		// For now we assume result has only 0 or 1 partition
		result = join(result, rhs_block);
		if (not result.is_satisfiable())
			return SolutionSet();
	}

	return result;
}

Unify::SolutionSet Unify::join(const SolutionSet& sol,
                               const TypedBlock& block) const
{
	SolutionSet result;
	for (const Partition& partition : sol) {
		SolutionSet jps = join(partition, block);
		result.insert(jps.begin(), jps.end());
	}
	return result;
}

Unify::SolutionSet Unify::join(const Partition& partition,
                               const TypedBlock& block) const
{
	// Find all partition blocks that have elements in common with block
	TypedBlockSeq common_blocks;
	for (const TypedBlock& p_block : partition)
		if (not has_empty_intersection(block.first, p_block.first))
			common_blocks.push_back(p_block);

	Partition jp(partition);
	if (common_blocks.empty()) {
		// If none then merely insert the independent block
		jp.insert(block);
		return SolutionSet({jp});
	} else {
		// Otherwise join block with all common blocks and replace
		// them by the result (if satisfiable, otherwise return the
		// empty solution set)
		TypedBlock j_block = join(common_blocks, block);
		if (is_satisfiable(j_block)) {
			for (const TypedBlock& rm : common_blocks)
				jp.erase(rm.first);
			jp.insert(j_block);

			// Perform the sub-unification of all common blocks with
			// block and join the solution set to jp
			SolutionSet sol = subunify(common_blocks, block);
			if (sol.is_satisfiable())
				return join(sol, jp);
		}
		return SolutionSet(false);
	}
}

Unify::TypedBlock Unify::join(const TypedBlockSeq& common_blocks,
                              const TypedBlock& block) const
{
	std::pair<Block, CHandle> result{block};
	for (const auto& c_block : common_blocks) {
		result =  join(result, c_block);
        // Abort if unsatisfiable
        if (not is_satisfiable(result))
            return result;
    }
	return result;
}

Unify::TypedBlock Unify::join(const TypedBlock& lhs, const TypedBlock& rhs) const
{
    OC_ASSERT(lhs.second and rhs.second, "Can only join 2 satisfiable blocks");
	return {set_union(lhs.first, rhs.first),
			type_intersection(lhs.second, rhs.second)};
}

Unify::SolutionSet Unify::subunify(const TypedBlockSeq& common_blocks,
                                   const TypedBlock& block) const
{
	// Form a set with all terms
	std::set<CHandle> all_chs(block.first);
	for (const TypedBlock& cb : common_blocks)
		all_chs.insert(cb.first.begin(), cb.first.end());

	// Build a set of all pairs of terms that may have not been
	// unified so far.
	std::set<CHandlePair> not_unified;
	// This function returns true iff both terms are in the given
	// block. If so it means they have already been unified.
	auto both_in_block = [](const CHandle& lch, const CHandle& rch,
	                        const TypedBlock& block) {
		return is_in(lch, block.first) and is_in(rch, block.first);
	};
	for (auto lit = all_chs.begin(); lit != all_chs.end(); ++lit) {
		for (auto rit = std::next(lit); rit != all_chs.end(); ++rit) {
			// Check if they are in block
			bool already_unified = both_in_block(*lit, *rit, block);
			// If not, then check if they are in one of the common
			// blocks
			if (not already_unified) {
				for (const TypedBlock& cb : common_blocks) {
					already_unified = both_in_block(*lit, *rit, cb);
					if (already_unified)
						break;
				}
			}
			if (not already_unified)
				not_unified.insert({*lit, *rit});
		}
	}

	// Unify all not unified yet terms
	return pairwise_unify(not_unified);
}

Unify::SolutionSet Unify::subunify(const TypedBlock& lhs,
                                   const TypedBlock& rhs) const
{
	return comb_unify(set_symmetric_difference(lhs.first, rhs.first));
}

bool Unify::is_satisfiable(const TypedBlock& block) const
{
	return (bool)block.second;
}

bool unifiable(const Handle& lhs, const Handle& rhs,
               const Handle& lhs_vardecl, const Handle& rhs_vardecl)
{
	Unify unify(lhs, rhs, lhs_vardecl, rhs_vardecl);
	return unify().is_satisfiable();
}

bool hm_content_eq(const HandleMap& lhs, const HandleMap& rhs)
{
	if (lhs.size() != rhs.size())
		return false;

	auto lit = lhs.begin();
	auto rit = rhs.begin();
	while (lit != lhs.end()) {
		if (not content_eq(lit->first, rit->first)
		   or not content_eq(lit->second, rit->second))
			return false;
		++lit; ++rit;
	}
	return true;
}

bool hchm_content_eq(const Unify::HandleCHandleMap& lhs,
                     const Unify::HandleCHandleMap& rhs)
{
	if (lhs.size() != rhs.size())
		return false;

	auto lit = lhs.begin();
	auto rit = rhs.begin();
	while (lit != lhs.end()) {
		if (not content_eq(lit->first, rit->first)
		    or lit->second != rit->second)
			return false;
		++lit; ++rit;
	}
	return true;
}

bool ts_content_eq(const Unify::TypedSubstitution& lhs,
                   const Unify::TypedSubstitution& rhs)
{
	return lhs.first.size() == rhs.first.size()
		and hchm_content_eq(lhs.first, rhs.first)
		and content_eq(lhs.second, rhs.second);
}

bool tss_content_eq(const Unify::TypedSubstitutions& lhs,
                    const Unify::TypedSubstitutions& rhs)
{
	if (lhs.size() != rhs.size())
		return false;

	auto lit = lhs.begin();
	auto rit = rhs.begin();
	while (lit != lhs.end()) {
		if (not ts_content_eq(*lit, *rit))
			return false;
		++lit; ++rit;
	}
	return true;
}

HandleMap strip_context(const Unify::HandleCHandleMap& hchm)
{
	HandleMap result;
	for (auto& el : hchm) {
		const Context& ctx = el.second.context;
		Handle val = el.second.handle;

		// Insert quotation links if necessary
		for (int i = 0; i < ctx.quotation.level(); i++) {
			if (i == 0 and ctx.quotation.is_locally_quoted())
				val = Handle(createLink(LOCAL_QUOTE_LINK, val));
			else
				val = Handle(createLink(QUOTE_LINK, val));
		}

		// Recreate variable to value mapping without context
		result.insert({el.first, val});
	}
	return result;
}

/**
 * Generate a VariableList of the free variables of a given contextual
 * atom ch.
 */
VariableListPtr gen_varlist(const Unify::CHandle& ch)
{
	HandleSet free_vars = ch.get_free_variables();
	return createVariableList(HandleSeq(free_vars.begin(), free_vars.end()));
}

Unify::CHandle Unify::type_intersection(const CHandle& lch, const CHandle& rch) const
{
	if (inherit(lch, rch))
		return lch;
	if (inherit(rch, lch))
		return rch;
	return Handle::UNDEFINED;
}

TypeSet Unify::simplify_type_union(TypeSet& type) const
{
	return {}; // TODO: do we really need that?
}

TypeSet Unify::get_union_type(const Handle& h) const
{
	const VariableTypeMap& vtm = _variables._simple_typemap;
	auto it = vtm.find(h);
	if (it == vtm.end() or it->second.empty())
		return {ATOM};
	else {
		return it->second;
	}
}

bool Unify::inherit(const CHandle& lch, const CHandle& rch) const
{
	return inherit(lch.handle, rch.handle, lch.context, rch.context);
}

bool Unify::inherit(const Handle& lh, const Handle& rh,
                    Context lc, Context rc) const
{
	Type lt = lh->get_type();
	Type rt = rh->get_type();

	// Recursive cases

	// Consume quotations
	if (lc.quotation.consumable(lt)) {
		lc.quotation.update(lt);
		return inherit(lh->getOutgoingAtom(0), rh, lc, rc);
	}
	if (rc.quotation.consumable(rt)) {
		rc.quotation.update(rt);
		return inherit(lh, rh->getOutgoingAtom(0), lc, rc);
	}

	// If both are links then check that the outgoings of lhs inherit
	// the outgoings of rhs.
	if (lh->is_link() and rh->is_link() and (lt == rt)) {
		if (lh->get_arity() == rh->get_arity()) {
			for (size_t i = 0; i < lh->get_arity(); i++) {
				if (not inherit(lh->getOutgoingAtom(i),
				                rh->getOutgoingAtom(i),
				                lc, rc))
					return false;
			}
			return true;
		} else return false;
	}

	// Base cases

	// If they are equal then lh trivial inherits from rh
	if (lh == rh)
		return true;

	// If both are free variables then look at their types (only
	// simple types are considered for now).
	if (lc.is_free_variable(lh) and rc.is_free_variable(rh))
		return inherit(get_union_type(lh), get_union_type(rh));

	// If only rh is a free variable, if its in _variable then check
	// whether lh type inherits from it (using Variables::is_type),
	// otherwise assume rh is the top type and thus anything inherits
	// from it.
	if (rc.is_free_variable(rh) or rt == GLOB_NODE)
        return not _variables.is_in_varset(rh) or _variables.is_type(rh, lh);

	return false;
}

bool Unify::inherit(Type lhs, Type rhs) const
{
	return nameserver().isA(lhs, rhs);
}

bool Unify::inherit(Type lhs, const TypeSet& rhs) const
{
	for (Type ty : rhs)
		if (inherit(lhs, ty))
			return true;
	return false;
}

bool Unify::inherit(const TypeSet& lhs, const TypeSet& rhs) const
{
	for (Type ty : lhs)
		if (not inherit(ty, rhs))
			return false;
	return true;
}

bool Unify::contain_glob(const Handle &handle) const {
	for (const Handle& h : handle->getOutgoingSet()) {
		if (h->get_type() == GLOB_NODE) return true;
	}
	return false;
}

Variables merge_variables(const Variables& lhs, const Variables& rhs)
{
	Variables new_vars(lhs);
	new_vars.extend(rhs);
	return new_vars;
}

Handle merge_vardecl(const Handle& lhs_vardecl, const Handle& rhs_vardecl)
{
	if (not lhs_vardecl)
		return rhs_vardecl;
	if (not rhs_vardecl)
		return lhs_vardecl;

	VariableList
		lhs_vl(lhs_vardecl),
		rhs_vl(rhs_vardecl);

	Variables new_vars =
		merge_variables(lhs_vl.get_variables(), rhs_vl.get_variables());
	return new_vars.get_vardecl();
}

std::string oc_to_string(const Unify::CHandle& ch, const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "context:" << std::endl
	   << oc_to_string(ch.context, indent + OC_TO_STRING_INDENT)
	   << indent << "atom:" << std::endl
	   << oc_to_string(ch.handle, indent + OC_TO_STRING_INDENT);
	return ss.str();
}

std::string oc_to_string(const Unify::Block& pb, const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "size = " << pb.size() << std::endl;
	int i = 0;
	for (const auto& el : pb)
		ss << indent << "catom[" << i++ << "]:" << std::endl
		   << oc_to_string(el, indent + OC_TO_STRING_INDENT);
	return ss.str();
}

std::string oc_to_string(const Unify::TypedBlock& tb, const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "block:" << std::endl
	   << oc_to_string(tb.first, indent + OC_TO_STRING_INDENT)
	   << indent << "type:" << std::endl
	   << oc_to_string(tb.second, indent + OC_TO_STRING_INDENT);
	return ss.str();
}

std::string oc_to_string(const Unify::TypedBlockSeq& tbs, const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "size = " << tbs.size() << std::endl;
	for (size_t i = 0; i < tbs.size(); i++)
		ss << indent << "typed block[" << i << "]:" << std::endl
		   << oc_to_string(tbs[i], indent + OC_TO_STRING_INDENT);
	return ss.str();
}

std::string oc_to_string(const Unify::Partition& up, const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "size = " << up.size() << std::endl;
	int i = 0;
	for (const auto& p : up) {
		ss << indent << "block[" << i << "]:" << std::endl
		   << oc_to_string(p.first, indent + OC_TO_STRING_INDENT)
		   << indent << "type[" << i << "]:" << std::endl
		   << oc_to_string(p.second, indent + OC_TO_STRING_INDENT);
		i++;
	}
	return ss.str();
}

std::string oc_to_string(const Unify::Partitions& par, const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "size = " << par.size() << std::endl;
	int i = 0;
	for (const auto& el : par) {
		ss << indent << "typed partition[" << i << "]:"
		   << std::endl << oc_to_string(el, indent + OC_TO_STRING_INDENT);
		i++;
	}
	return ss.str();
}

std::string oc_to_string(const Unify::HandleCHandleMap& hchm,
                         const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "size = " << hchm.size() << std::endl;
	int i = 0;
	for (const auto& hch : hchm) {
		ss << indent << "atom[" << i << "]:" << std::endl
		   << oc_to_string(hch.first, indent + OC_TO_STRING_INDENT);
		ss << indent << "catom[" << i << "]:" << std::endl
		   << oc_to_string(hch.second, indent + OC_TO_STRING_INDENT);
		i++;
	}
	return ss.str();
}

std::string oc_to_string(const Unify::HandleCHandleMap::value_type& hch,
                         const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "atom:" << std::endl
	   << oc_to_string(hch.first, indent + OC_TO_STRING_INDENT);
	ss << indent << "catom:" << std::endl
	   << oc_to_string(hch.second, indent + OC_TO_STRING_INDENT);
	return ss.str();
}

std::string oc_to_string(const Unify::TypedSubstitution& ts,
                         const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "substitution:" << std::endl
	   << oc_to_string(ts.first, indent + OC_TO_STRING_INDENT)
	   << indent << "vardecl:" << std::endl
	   << oc_to_string(ts.second, indent + OC_TO_STRING_INDENT);
	return ss.str();
}

std::string oc_to_string(const Unify::TypedSubstitutions::value_type& ts,
                         const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "substitution:" << std::endl
	   << oc_to_string(ts.first, indent + OC_TO_STRING_INDENT)
	   << indent << "vardecl:" << std::endl
	   << oc_to_string(ts.second, indent + OC_TO_STRING_INDENT);
	return ss.str();
}

std::string oc_to_string(const Unify::TypedSubstitutions& tss,
                         const std::string& indent)
{
	std::stringstream ss;
	ss << indent << "size = " << tss.size() << std::endl;
	int i = 0;
	for (const auto& ts : tss) {
		ss << indent << "typed substitution[" << i << "]:" << std::endl
		   << oc_to_string(ts, indent + OC_TO_STRING_INDENT);
		i++;
	}
	return ss.str();
}

} // namespace opencog
