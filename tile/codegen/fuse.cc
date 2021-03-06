// Copyright 2018, Intel Corporation

#include "tile/codegen/fuse.h"

#include <boost/format.hpp>

#include "base/util/stream_container.h"
#include "base/util/throw.h"
#include "tile/codegen/localize.h"
#include "tile/codegen/tile.h"

namespace vertexai {
namespace tile {
namespace codegen {

using namespace stripe;  // NOLINT

boost::optional<FusionPlan> ComputeFusionPlan(const Block& a, const Block& b, const std::string& buf_name) {
  IVLOG(3, "ComputeFusionPlan for " << buf_name << " between " << a.name << " and " << b.name);
  FusionPlan plan;
  plan.tile_a = TileShape(a.idxs.size(), 1);
  plan.tile_b = TileShape(b.idxs.size(), 1);
  // This is quite hueristic right now, but still beats our prior implementation
  auto it_a = a.ref_by_from(buf_name, false);
  if (it_a == a.refs.end()) {
    IVLOG(3, "ComputeFusionPlan: buffer name unknown in block a");
    return boost::none;
  }
  auto it_b = b.ref_by_from(buf_name, false);
  if (it_b == b.refs.end()) {
    IVLOG(3, "ComputeFusionPlan: buffer name unknown in block b");
    return boost::none;
  }
  assert(it_a->access.size() == it_b->access.size());
  for (size_t i = 0; i < it_a->access.size(); i++) {
    const Affine& poly_a = it_a->access[i];
    const Affine& poly_b = it_b->access[i];
    if (poly_a == 0 && poly_b == 0) {
      continue;
    }
    if (poly_a.getMap().size() != 1 || poly_a.getMap().begin()->first.empty()) {
      IVLOG(3, "ComputeFusionPlan: complex access in a: " << poly_a.toString());
      return boost::none;
    }
    if (poly_b.getMap().size() != 1 || poly_b.getMap().begin()->first.empty()) {
      IVLOG(3, "ComputeFusionPlan: complex access in b" << poly_b.toString());
      return boost::none;
    }
    std::string idx_a = poly_a.getMap().begin()->first;
    std::string idx_b = poly_b.getMap().begin()->first;
    if (plan.remap_a.find(idx_a) != plan.remap_a.end()) {
      IVLOG(3, "ComputeFusionPlan: duplicate index");
      return boost::none;
    }
    int64_t mul_a = poly_a[idx_a];
    int64_t mul_b = poly_b[idx_b];
    if (mul_a % mul_b != 0) {
      IVLOG(3, "ComputeFusionPlan: uneven index division");
      return boost::none;
    }
    for (size_t i = 0; i < b.idxs.size(); i++) {
      if (b.idxs[i].name == idx_b) {
        plan.tile_b[i] = mul_a / mul_b;
      }
    }
    plan.remap_a.emplace(idx_a, idx_a);
    plan.remap_b.emplace(idx_b, idx_a);
  }
  if (a.constraints != b.constraints) {
    IVLOG(3, "ComputeFusionPlan: incompatible constraints");
    IVLOG(4, "    a: " << StreamContainer(a.constraints));
    IVLOG(4, "    b: " << StreamContainer(b.constraints));
    return boost::none;
  }
  for (const auto& constraint : a.constraints) {
    for (const auto& term : constraint.getMap()) {
      auto it = plan.remap_a.find(term.first);
      if (it == plan.remap_a.end()) {
        plan.remap_a.emplace(term.first, term.first);
      }
    }
  }
  for (const auto& constraint : b.constraints) {
    for (const auto& term : constraint.getMap()) {
      auto it = plan.remap_b.find(term.first);
      if (it == plan.remap_b.end()) {
        plan.remap_b.emplace(term.first, term.first);
      }
    }
  }
  return plan;
}

void FlattenTrivial(stripe::Block* outer) {
  IVLOG(4, "FlattenTrivial before:\n" << *outer);
  auto it = outer->stmts.begin();
  while (it != outer->stmts.end()) {
    auto inner = Block::Downcast(*it);
    // Skip non blocks
    if (!inner) {
      IVLOG(4, "FlattenTrivial: skip> non-block");
      ++it;
      continue;
    }
    uint64_t range = 1;
    for (const auto& idx : inner->idxs) {
      range *= idx.range;
    }
    if (range != 1) {
      IVLOG(4, "FlattenTrivial: skip> range != 1");
      ++it;
      continue;
    }
    bool renames = false;
    for (const auto& ref : inner->refs) {
      if (ref.from != "" && ref.into != ref.from) {
        renames = true;
      }
    }
    // TODO: renames technically can be applied to inner statements,
    // but it's really annoying!
    if (renames) {
      IVLOG(4, "FlattenTrivial: skip> renames");
      ++it;
      continue;
    }
    // Move out inner statements
    for (auto& stmt : inner->stmts) {
      auto deep = Block::Downcast(stmt);
      if (deep) {
        // Rewrite any copied down indexes
        for (auto& idx : deep->idxs) {
          std::vector<std::string> names;
          for (const auto& item : idx.affine.getMap()) {
            names.push_back(item.first);
          }
          for (const auto& name : names) {
            idx.affine.substitute(name, inner->idx_by_name(name)->affine);
          }
        }
      }
      outer->stmts.insert(it, stmt);
    }
    auto it_old = it;
    ++it;
    outer->stmts.erase(it_old);
  }

  IVLOG(4, "FlattenTrivial after:\n" << *outer);
}

std::shared_ptr<Block> FusionRefactor(const stripe::Block& orig,                          //
                                      const std::map<std::string, std::string>& mapping,  //
                                      const TileShape& tile) {
  IVLOG(3, "FusionRefactor:\n" << orig);
  IVLOG(3, "mapping: " << StreamContainer(mapping) << ", tile: " << tile);
  // Possibly tile
  auto tiled = std::make_shared<Block>(orig);
  ApplyTile(tiled.get(), tile, true, true);
  // Make empty inner and outer blocks, and put inner into outer
  auto outer = std::make_shared<Block>();
  outer->name = tiled->name;
  // This is safe to do because we check whether constraints are equivalent in ComputeFusionPlan
  outer->constraints = tiled->constraints;
  auto inner = std::make_shared<Block>();
  inner->name = tiled->name;
  outer->tags = tiled->tags;
  outer->stmts.push_back(inner);
  // Move / rename each index to the appropriate block
  for (const auto& idx : tiled->idxs) {
    auto it = mapping.find(idx.name);
    if (it == mapping.end()) {
      IVLOG(3, "New idx: " << idx.name);
      inner->idxs.push_back(idx);
    } else {
      IVLOG(3, "Existing idx: " << idx.name);
      inner->idxs.emplace_back(Index{idx.name, 1, it->second});
      outer->idxs.push_back(idx);
      outer->idxs.back().name = it->second;
    }
  }
  // Sort outer indexes by names
  std::sort(outer->idxs.begin(), outer->idxs.end(), [](const Index& a, const Index& b) { return a.name < b.name; });
  // Copy constraints to inner block
  inner->constraints = tiled->constraints;
  // Copy statements to the inner block
  inner->stmts = tiled->stmts;
  // Copy refinements to both blocks
  outer->refs = tiled->refs;
  inner->refs = tiled->refs;
  // Rename mapped, and remove unmapped access elements from outer refinements
  // Also expand sizes base on inner indexes that have been removed.
  for (auto& ref : outer->refs) {
    for (size_t i = 0; i < ref.access.size(); i++) {
      auto& acc = ref.access[i];
      int64_t max_val = ref.interior_shape.dims[i].size - 1;
      Affine affine = acc.constant();
      for (const auto& kvp : acc.getMap()) {
        auto it = mapping.find(kvp.first);
        if (it == mapping.end()) {
          if (kvp.first != "") {
            if (kvp.second < 0) {
              throw_with_trace(std::runtime_error("FusionRefactor: Unable to handle negative strides"));
            }
            max_val += (tiled->idx_by_name(kvp.first)->range - 1) * kvp.second;
          }
          continue;
        }
        affine += Affine(it->second, kvp.second);
      }
      ref.interior_shape.dims[i].size = max_val + 1;
      acc = affine;
    }
  }
  // Remove mapped access elements from inner refinements
  for (auto& ref : inner->refs) {
    // Rename from to match outer into
    ref.from = ref.into;
    // Update accesses
    for (auto& acc : ref.access) {
      Affine affine;
      for (const auto& kvp : acc.getMap()) {
        if (kvp.first != "" && !mapping.count(kvp.first)) {
          affine += Affine(kvp.first, kvp.second);
        }
      }
      acc = affine;
    }
  }
  // Remove any trivial loops remaining
  FlattenTrivial(outer.get());
  // Return final result
  IVLOG(3, "Refactor output:\n" << *outer);
  return outer;
}

bool FuseBlocks(const AliasMap& scope, Block* block_a, Block* block_b) {
  // If indexes don't match, fail
  if (block_a->idxs != block_b->idxs) {
    IVLOG(3, "Fuse failed due to mismatched indexes");
    return false;
  }
  // If constraints don't match, fail
  if (block_a->constraints != block_b->constraints) {
    IVLOG(3, "Fuse failed due to mismatched constraints");
    return true;
  }
  // Make AliasMaps for the two blocks
  AliasMap a_map(scope, block_a);
  AliasMap b_map(scope, block_b);
  // Start by copying A's reference across
  auto tmp = std::make_shared<Block>();
  tmp->refs = block_a->refs;
  // Walk over refinements in B and move them across
  // Rename duplicate refinements in B to their name in A
  // Otherwise make a new unique name (keeping original if possible)
  std::map<std::string, std::string> remap_b;
  for (const auto& new_ref : block_b->refs) {
    // If it's a local, always safe to copy if across
    // Check if b matches something in the existing block
    bool merged = false;
    for (auto& old_ref : block_a->refs) {
      auto atype = AliasInfo::Compare(a_map.at(old_ref.into), b_map.at(new_ref.into));
      if (atype == AliasType::Partial) {
        // Conflict, if either do any writing, we have a problem
        if (IsWriteDir(new_ref.dir) || IsWriteDir(old_ref.dir)) {
          IVLOG(3, "Fuse failed due to mismatched aliases: " << old_ref.into << " vs " << new_ref.into);
          return false;  // Fuse will not work, bail
        }
      } else if (atype == AliasType::Exact) {
        remap_b[new_ref.into] = old_ref.into;
        old_ref.dir = UnionDir(old_ref.dir, new_ref.dir);
        merged = true;
        break;
      }
    }
    if (!merged) {
      // Copy across as a new ref
      std::string new_name = tmp->unique_ref_name(new_ref.into);
      remap_b[new_ref.into] = new_name;
      auto ref_it = tmp->refs.insert(tmp->refs.end(), new_ref);
      ref_it->into = new_name;
    }
  }
  // We are now safe (cannot fail), move new reference over A's
  std::swap(block_a->refs, tmp->refs);
  if (!block_a->name.empty()) {
    block_a->name = str(boost::format("%s+%s") % block_a->name % block_b->name);
  } else if (!block_b->name.empty()) {
    block_a->name = block_b->name;
  }
  // Load all the scalars that exist as of block A
  std::set<std::string> all_scalars;
  std::map<std::string, std::string> scalar_rename;
  for (const auto& stmt : block_a->stmts) {
    for (const auto& name : stmt->scalar_defs()) {
      all_scalars.emplace(name);
    }
  }
  auto def_scalar = [&](const std::string& orig) -> std::string {
    if (all_scalars.count(orig) == 0) {
      all_scalars.emplace(orig);
      scalar_rename[orig] = orig;
      return orig;
    }
    for (size_t i = 0; true; i++) {
      std::string with_suffix = orig + "_" + std::to_string(i);
      if (all_scalars.count(with_suffix) == 0) {
        all_scalars.emplace(with_suffix);
        scalar_rename[orig] = with_suffix;
        return with_suffix;
      }
    }
    return "";
  };
  // Now move across statements, updating references/scalars as we do:
  for (const auto& stmt : block_b->stmts) {
    switch (stmt->kind()) {
      case StmtKind::Load: {
        auto op = Load::Downcast(stmt);
        op->into = def_scalar(op->into);
        op->from = remap_b.at(op->from);
      } break;
      case StmtKind::Store: {
        auto op = Store::Downcast(stmt);
        op->into = remap_b.at(op->into);
        op->from = scalar_rename.at(op->from);
      } break;
      case StmtKind::Special: {
        auto op = Special::Downcast(stmt);
        for (auto& in : op->inputs) {
          in = remap_b.at(in);
        }
        for (auto& out : op->outputs) {
          out = remap_b.at(out);
        }
      } break;
      case StmtKind::Block: {
        auto op = Block::Downcast(stmt);
        for (auto& ref : op->refs) {
          ref.from = remap_b.at(ref.from);
        }
      } break;
      case StmtKind::Constant: {
        auto op = Constant::Downcast(stmt);
        op->name = def_scalar(op->name);
      } break;
      case StmtKind::Intrinsic: {
        auto op = Intrinsic::Downcast(stmt);
        for (auto& in : op->inputs) {
          in = scalar_rename.at(in);
        }
        for (auto& out : op->outputs) {
          out = def_scalar(out);
        }
      } break;
    }
    block_a->stmts.push_back(stmt);
  }
  // All is well
  return true;
}

void FusionInner(const AliasMap& scope, Block* block, FusionStrategy* strategy) {
  // Start with the first statement, and keep tying to fuse until you can't anymore, then move to the next
  auto it = block->stmts.begin();
  while (it != block->stmts.end()) {
    // If it's not a block, forget it!
    if ((*it)->kind() != StmtKind::Block) {
      ++it;
      continue;
    }
    while (true) {
      // Get block everytime in case it's updated
      auto block1 = Block::Downcast(*it);
      IVLOG(3, "Attempting fusion on block:\n" << block1->name);
      // Get the next statement
      auto it_next = it;
      it_next++;
      // If there is no next statement, I'm done with this block
      if (it_next == block->stmts.end()) {
        break;
      }
      // Convert to block
      auto block2 = Block::Downcast(*it_next);
      // If it's not a block, forget it
      if (!block2) {
        break;
      }
      // Get the list of outputs for this block
      std::set<std::string> outs_for_fuse;
      for (const auto& ro : block1->ref_outs()) {
        IVLOG(3, "Considering output: " << ro->from);
        outs_for_fuse.emplace(ro->from);
      }
      IVLOG(3, "Outs for fuse size: " << outs_for_fuse.size());
      std::string fuse_on = "";
      // Check if it's a match to any of the inputs on the next block
      for (const auto& ri : block2->ref_ins()) {
        IVLOG(3, "Considering input: " << ri->from);
        if (outs_for_fuse.count(ri->from)) {
          fuse_on = ri->from;
          break;
        }
      }
      // Nothing to fuse on, done with this block
      if (fuse_on == "") {
        IVLOG(3, "Nothing to fuse on");
        break;
      }
      IVLOG(3, "Fuse on = " << fuse_on);
      // Compute a fusion plan for the two blocks, if fails, give up
      auto plan = ComputeFusionPlan(*block1, *block2, fuse_on);
      if (!plan) {
        IVLOG(3, "Fusion plan failed");
        break;
      }
      // Now call the strategy to see if we should fuse
      if (!strategy->AttemptFuse(*block, *block1, *block2)) {
        IVLOG(3, "Fusion denied by strategy");
        break;
      }
      // Do the appropriate refactors
      auto refactor1 = FusionRefactor(*block1, plan->remap_a, plan->tile_a);
      auto refactor2 = FusionRefactor(*block2, plan->remap_b, plan->tile_b);
      // IVLOG(3, "Fusion refactor 1:\n" << *refactor1);
      // IVLOG(3, "Fusion refactor 2:\n" << *refactor2);
      // Try the actual fusion
      if (!FuseBlocks(scope, refactor1.get(), refactor2.get())) {
        strategy->OnFailed();
        IVLOG(3, "Actual fusion failed");
        break;
      }
      IVLOG(3, "Fused block:\n" << *refactor1);
      // If it worked, update
      *it = refactor1;
      block->stmts.erase(it_next);
      strategy->OnFused(scope, refactor1.get(), *block1, *block2);
    }
    it++;
  }
}

struct FusionPassOptions {
  Tags parent_reqs;
  Tags a_block_reqs;
  Tags b_block_reqs;
  Tags fused_set;
};

class TagFusionStrategy : public FusionStrategy {
 public:
  explicit TagFusionStrategy(const FusionPassOptions& options) : options_(options) {}
  bool AttemptFuse(const stripe::Block& parent, const stripe::Block& a, const stripe::Block& b) {
    return parent.has_tags(options_.parent_reqs) &&  //
           a.has_tags(options_.a_block_reqs) &&      //
           b.has_tags(options_.b_block_reqs);
  }
  void OnFailed() {}
  void OnFused(const AliasMap& outer, stripe::Block* block, const stripe::Block& a, const stripe::Block& b) {
    block->add_tags(options_.fused_set);
  }

 private:
  const FusionPassOptions& options_;
};

static void FusionPassRecurse(const AliasMap& map, stripe::Block* block, TagFusionStrategy* strategy) {
  FusionInner(map, block, strategy);
  for (const auto& stmt : block->stmts) {
    auto inner = Block::Downcast(stmt);
    if (inner) {
      AliasMap inner_map(map, inner.get());
      FusionPassRecurse(inner_map, inner.get(), strategy);
    }
  }
}

void FusionPass(stripe::Block* root, const proto::FusionPass& options) {
  FusionPassOptions fopts = {
      FromProto(options.parent_reqs()),  // parent_reqs
      FromProto(options.a_reqs()),       // a_block_reqs
      FromProto(options.b_reqs()),       // b_block_reqs
      FromProto(options.fused_set())     // fused_set
  };
  AliasMap base;
  AliasMap root_map(base, root);
  // Check if we should fuse this block
  TagFusionStrategy strategy(fopts);
  FusionPassRecurse(root_map, root, &strategy);
}

}  // namespace codegen
}  // namespace tile
}  // namespace vertexai
