/*
 * Copyright (c) 2018 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "rellic/AST/InferenceRule.h"
#include "rellic/AST/LoopRefine.h"

namespace rellic {

namespace {

using namespace clang::ast_matchers;

// Matches `while(1)`, `if(1)`, etc.
static const auto cond_true = hasCondition(integerLiteral(equals(true)));
// Matches `{ break; }`
static const auto comp_break =
    compoundStmt(has(breakStmt()), statementCountIs(1));

class WhileRule : public InferenceRule {
 public:
  WhileRule()
      : InferenceRule(whileStmt(
            stmt().bind("while"), cond_true,
            hasBody(compoundStmt(
                has(ifStmt(stmt().bind("if"), hasThen(comp_break))))))) {}

  void run(const MatchFinder::MatchResult &result) {
    auto loop = result.Nodes.getNodeAs<clang::WhileStmt>("while");
    auto body = clang::cast<clang::CompoundStmt>(loop->getBody());
    auto ifstmt = result.Nodes.getNodeAs<clang::IfStmt>("if");
    if (body->body_front() == ifstmt) {
      match = loop;
    }
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto loop = clang::dyn_cast<clang::WhileStmt>(stmt);

    CHECK(loop && loop == match)
        << "Substituted WhileStmt is not the matched WhileStmt!";

    auto comp = clang::cast<clang::CompoundStmt>(loop->getBody());
    auto cond = clang::cast<clang::IfStmt>(comp->body_front())->getCond();
    std::vector<clang::Stmt *> new_body(comp->body_begin() + 1,
                                        comp->body_end());

    return CreateWhileStmt(ctx, CreateNotExpr(ctx, cond),
                           CreateCompoundStmt(ctx, new_body));
  }
};

class DoWhileRule : public InferenceRule {
 public:
  DoWhileRule()
      : InferenceRule(whileStmt(
            stmt().bind("while"), cond_true,
            hasBody(compoundStmt(
                has(ifStmt(stmt().bind("if"), hasThen(comp_break))))))) {}

  void run(const MatchFinder::MatchResult &result) {
    auto loop = result.Nodes.getNodeAs<clang::WhileStmt>("while");
    auto body = clang::cast<clang::CompoundStmt>(loop->getBody());
    auto ifstmt = result.Nodes.getNodeAs<clang::IfStmt>("if");
    if (body->body_back() == ifstmt) {
      match = loop;
    }
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto loop = clang::dyn_cast<clang::WhileStmt>(stmt);

    CHECK(loop && loop == match)
        << "Substituted WhileStmt is not the matched WhileStmt!";

    auto comp = clang::cast<clang::CompoundStmt>(loop->getBody());
    auto cond = clang::cast<clang::IfStmt>(comp->body_back())->getCond();
    std::vector<clang::Stmt *> new_body(comp->body_begin(),
                                        comp->body_end() - 1);

    return CreateDoStmt(ctx, CreateNotExpr(ctx, cond),
                        CreateCompoundStmt(ctx, new_body));
  }
};

class NestedDoWhileRule : public InferenceRule {
 private:
  bool matched = false;

 public:
  NestedDoWhileRule()
      : InferenceRule(
            whileStmt(stmt().bind("while"), cond_true,
                      hasBody(compoundStmt(findAll(ifStmt(
                          stmt().bind("if"), hasThen(has(breakStmt())))))))) {}

  void run(const MatchFinder::MatchResult &result) {
    if (!matched) {
      auto loop = result.Nodes.getNodeAs<clang::WhileStmt>("while");
      auto body = clang::cast<clang::CompoundStmt>(loop->getBody());
      auto ifstmt = result.Nodes.getNodeAs<clang::IfStmt>("if");
      if (body->body_back() == ifstmt) {
        match = loop;
      }
    } else {
      match = nullptr;
    }
    matched = true;
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto loop = clang::dyn_cast<clang::WhileStmt>(stmt);

    CHECK(loop && loop == match)
        << "Substituted WhileStmt is not the matched WhileStmt!";
    auto comp = clang::cast<clang::CompoundStmt>(loop->getBody());
    auto cond = clang::cast<clang::IfStmt>(comp->body_back());

    std::vector<clang::Stmt *> do_body(comp->body_begin(),
                                       comp->body_end() - 1);

    auto do_cond = CreateNotExpr(ctx, cond->getCond());
    auto do_stmt = CreateDoStmt(ctx, do_cond, CreateCompoundStmt(ctx, do_body));

    std::vector<clang::Stmt *> while_body({do_stmt, cond->getThen()});
    return CreateWhileStmt(ctx, loop->getCond(),
                           CreateCompoundStmt(ctx, while_body));
  }
};

class LoopToSeq : public InferenceRule {
 public:
  LoopToSeq()
      : InferenceRule(
            whileStmt(stmt().bind("while"), cond_true,
                      hasBody(compoundStmt(hasAnySubstatement(anyOf(
                          ifStmt(stmt().bind("if"), hasThen(has(breakStmt())),
                                 hasElse(has(breakStmt()))),
                          breakStmt())))))) {}

  void run(const MatchFinder::MatchResult &result) {
    auto loop = result.Nodes.getNodeAs<clang::WhileStmt>("while");
    if (auto ifstmt = result.Nodes.getNodeAs<clang::IfStmt>("if")) {
      auto body = clang::cast<clang::CompoundStmt>(loop->getBody());
      if (body->body_back() == ifstmt) {
        match = loop;
      }
    } else {
      match = loop;
    }
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto loop = clang::dyn_cast<clang::WhileStmt>(stmt);

    CHECK(loop && loop == match)
        << "Substituted WhileStmt is not the matched WhileStmt!";

    auto loop_body = clang::cast<clang::CompoundStmt>(loop->getBody());

    std::vector<clang::Stmt *> new_body(loop_body->body_begin(),
                                        loop_body->body_end());

    if (auto ifstmt = clang::dyn_cast<clang::IfStmt>(loop_body->body_back())) {
      std::vector<clang::Stmt *> branches(
          {ifstmt->getThen(), ifstmt->getElse()});
      for (auto &branch : branches) {
        std::vector<clang::Stmt *> new_branch_body;
        if (auto branch_body = clang::dyn_cast<clang::CompoundStmt>(branch)) {
          for (auto stmt : branch_body->body()) {
            if (clang::isa<clang::BreakStmt>(stmt)) {
              break;
            }
            new_branch_body.push_back(stmt);
          }
        }
        branch = CreateCompoundStmt(ctx, new_branch_body);
      }
      ifstmt->setThen(branches[0]);
      ifstmt->setElse(branches[1]);
    } else {
      new_body.pop_back();
    }

    return CreateCompoundStmt(ctx, new_body);
  }
};

static const auto has_break = hasDescendant(breakStmt());

class CondToSeqRule : public InferenceRule {
 public:
  CondToSeqRule()
      : InferenceRule(whileStmt(
            stmt().bind("while"), cond_true,
            hasBody(compoundStmt(
                has(ifStmt(hasThen(unless(has_break)), hasElse(has_break))),
                statementCountIs(1))))) {}

  void run(const MatchFinder::MatchResult &result) {
    match = result.Nodes.getNodeAs<clang::WhileStmt>("while");
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto loop = clang::dyn_cast<clang::WhileStmt>(stmt);

    CHECK(loop && loop == match)
        << "Substituted WhileStmt is not the matched WhileStmt!";

    auto body = clang::cast<clang::CompoundStmt>(loop->getBody());
    auto ifstmt = clang::cast<clang::IfStmt>(body->body_front());
    auto inner_loop =
        CreateWhileStmt(ctx, ifstmt->getCond(), ifstmt->getThen());
    std::vector<clang::Stmt *> new_body({inner_loop});
    if (auto comp = clang::dyn_cast<clang::CompoundStmt>(ifstmt->getElse())) {
      new_body.insert(new_body.end(), comp->body_begin(), comp->body_end());
    } else {
      new_body.push_back(ifstmt->getElse());
    }

    return CreateWhileStmt(ctx, loop->getCond(),
                           CreateCompoundStmt(ctx, new_body));
  }
};

class CondToSeqNegRule : public InferenceRule {
 public:
  CondToSeqNegRule()
      : InferenceRule(whileStmt(
            stmt().bind("while"), cond_true,
            hasBody(compoundStmt(
                has(ifStmt(hasThen(has_break), hasElse(unless(has_break)))),
                statementCountIs(1))))) {}

  void run(const MatchFinder::MatchResult &result) {
    match = result.Nodes.getNodeAs<clang::WhileStmt>("while");
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto loop = clang::dyn_cast<clang::WhileStmt>(stmt);

    CHECK(loop && loop == match)
        << "Substituted WhileStmt is not the matched WhileStmt!";

    auto body = clang::cast<clang::CompoundStmt>(loop->getBody());
    auto ifstmt = clang::cast<clang::IfStmt>(body->body_front());
    auto cond = CreateNotExpr(ctx, ifstmt->getCond());
    auto inner_loop = CreateWhileStmt(ctx, cond, ifstmt->getElse());
    std::vector<clang::Stmt *> new_body({inner_loop});
    if (auto comp = clang::dyn_cast<clang::CompoundStmt>(ifstmt->getThen())) {
      new_body.insert(new_body.end(), comp->body_begin(), comp->body_end());
    } else {
      new_body.push_back(ifstmt->getThen());
    }

    return CreateWhileStmt(ctx, loop->getCond(),
                           CreateCompoundStmt(ctx, new_body));
  }
};

}  // namespace

char LoopRefine::ID = 0;

LoopRefine::LoopRefine(clang::ASTContext &ctx, rellic::IRToASTVisitor &ast_gen)
    : ModulePass(LoopRefine::ID), ast_ctx(&ctx), ast_gen(&ast_gen) {}

bool LoopRefine::VisitWhileStmt(clang::WhileStmt *loop) {
  // DLOG(INFO) << "VisitWhileStmt";
  std::vector<InferenceRule *> rules;

  rules.push_back(new CondToSeqRule);
  rules.push_back(new CondToSeqNegRule);
  rules.push_back(new NestedDoWhileRule);
  rules.push_back(new LoopToSeq);
  rules.push_back(new WhileRule);
  rules.push_back(new DoWhileRule);

  auto sub = ApplyFirstMatchingRule(*ast_ctx, loop, rules);
  if (sub != loop) {
    substitutions[loop] = sub;
  }

  for (auto rule : rules) {
    delete rule;
  }

  return true;
}

bool LoopRefine::runOnModule(llvm::Module &module) {
  LOG(INFO) << "Rule-based loop refinement";
  Initialize();
  TraverseDecl(ast_ctx->getTranslationUnitDecl());
  return changed;
}

llvm::ModulePass *createLoopRefinePass(clang::ASTContext &ctx,
                                       rellic::IRToASTVisitor &gen) {
  return new LoopRefine(ctx, gen);
}
}  // namespace rellic