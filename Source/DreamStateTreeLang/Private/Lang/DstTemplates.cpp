// Copyright DreamMoon. All Rights Reserved.
//
// #include resolution and template expansion.
//
// Expansion works on expressions only. It visits every place where a value is written: the value of an `on`, `if`
// or `utility` clause, the nodes of a `do`, a transition's trigger and condition, call arguments, the values of
// section entries and of subtree parameter defaults, and the values inside `[ ... ]`. Four places hold a name that
// is not a value and are left alone, so a template can never be mistaken for them: the mode after `pick`, the
// target after `->`, the subtree or asset a `link` names (its arguments are values and do expand), and the bare
// `Value;` entries of a section such as `Tags`. The header arguments of the document are not visited either.

#include "Lang/DstTemplates.h"

#include "HAL/FileManager.h"
#include "Lang/DstParser.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::DreamStateTree::Lang
{
	namespace
	{
		// A template that (directly or not) calls itself would otherwise expand forever.
		constexpr int32 MaxExpansionDepth = 16;

		struct FTemplateDef
		{
			const FDstTemplateStmt* Stmt = nullptr;
			FString File;
		};

		// -- includes ---------------------------------------------------------------------------

		struct FIncludeWalker
		{
			FDstCompilationUnit& Unit;
			IDstIncludeLoader& Loader;
			FDstDiagnosticSink& Sink;
			// Paths compare without regard to case here (FString's == does), which is what a file system on
			// Windows does too; the file system loader also hands back the spelling the disk has.
			TSet<FString> Parsed;
			TArray<FString> Stack;

			FIncludeWalker(FDstCompilationUnit& InUnit, IDstIncludeLoader& InLoader, FDstDiagnosticSink& InSink)
				: Unit(InUnit), Loader(InLoader), Sink(InSink)
			{
			}

			void Walk(const FDstFile& File)
			{
				for (const TUniquePtr<FDstInclude>& Include : File.Includes)
				{
					// The parser has reported the missing path (DST2007).
					if (!Include || Include->Path.IsEmpty())
					{
						continue;
					}

					FString Resolved;
					FString Text;
					if (!Loader.Load(File.Path, Include->Path, Resolved, Text))
					{
						Sink.ErrorIn(TEXT("DST3201"), File.Path, Include->PathSpan,
							FString::Printf(TEXT("Cannot find the included file '%s'."), *Include->Path),
							TEXT("Paths are relative to the including file, then to each DTree/ root."));
						continue;
					}
					if (Stack.Contains(Resolved))
					{
						Sink.ErrorIn(TEXT("DST3202"), File.Path, Include->PathSpan,
							FString::Printf(TEXT("'%s' includes itself through a cycle: %s -> %s."),
								*Include->Path, *FString::Join(Stack, TEXT(" -> ")), *Resolved));
						continue;
					}
					if (Parsed.Contains(Resolved))
					{
						continue;
					}
					Parsed.Add(Resolved);

					FDstSourceText Source(Resolved, MoveTemp(Text));
					FDstDiagnosticSink LibrarySink(Resolved);
					TUniquePtr<FDstFile> Library = ParseDst(Source, LibrarySink);
					Sink.Append(MoveTemp(LibrarySink));

					if (!Library || !Library->Document || Library->Document->DocumentKind != EDstDocumentKind::StateTreeLibrary)
					{
						Sink.ErrorIn(TEXT("DST3203"), File.Path, Include->PathSpan,
							FString::Printf(TEXT("'%s' is not a library: an included file must hold a StateTreeLibrary(...) document."), *Include->Path),
							TEXT("Only .dth files written as StateTreeLibrary(Name=\"...\") { ... } can be included."));
						continue;
					}

					const FDstFile& Added = *Library;
					Unit.LibrarySources.Add(MoveTemp(Source));
					Unit.Libraries.Add(MoveTemp(Library));
					Stack.Push(Resolved);
					Walk(Added);
					Stack.Pop();
				}
			}
		};

		// -- expression traversal ---------------------------------------------------------------

		// Calls Visit on every child of an expression-level node (an expression, an argument, a metadata block or one
		// of its entries). Statements are not visited: expansion walks them itself, clause by clause.
		void ForEachChildNode(FDstNode& Node, TFunctionRef<void(FDstNode&)> Visit)
		{
			switch (Node.Kind)
			{
			case EDstNodeKind::MemberExpr:
				if (FDstExprPtr& Object = static_cast<FDstMemberExpr&>(Node).Object)
				{
					Visit(*Object);
				}
				break;
			case EDstNodeKind::CallExpr:
			{
				FDstCallExpr& Call = static_cast<FDstCallExpr&>(Node);
				if (Call.Callee)
				{
					Visit(*Call.Callee);
				}
				for (FDstArgumentPtr& Arg : Call.Args)
				{
					if (Arg)
					{
						Visit(*Arg);
					}
				}
				if (Call.Meta)
				{
					Visit(*Call.Meta);
				}
				break;
			}
			case EDstNodeKind::ListExpr:
				for (FDstExprPtr& Item : static_cast<FDstListExpr&>(Node).Items)
				{
					if (Item)
					{
						Visit(*Item);
					}
				}
				break;
			case EDstNodeKind::TupleExpr:
				for (FDstArgumentPtr& Item : static_cast<FDstTupleExpr&>(Node).Items)
				{
					if (Item)
					{
						Visit(*Item);
					}
				}
				break;
			case EDstNodeKind::UnaryExpr:
				if (FDstExprPtr& Operand = static_cast<FDstUnaryExpr&>(Node).Operand)
				{
					Visit(*Operand);
				}
				break;
			case EDstNodeKind::BinaryExpr:
			{
				FDstBinaryExpr& Binary = static_cast<FDstBinaryExpr&>(Node);
				if (Binary.Lhs)
				{
					Visit(*Binary.Lhs);
				}
				if (Binary.Rhs)
				{
					Visit(*Binary.Rhs);
				}
				break;
			}
			case EDstNodeKind::ParenExpr:
				if (FDstExprPtr& Inner = static_cast<FDstParenExpr&>(Node).Inner)
				{
					Visit(*Inner);
				}
				break;
			case EDstNodeKind::RefExpr:
				if (FDstExprPtr& Target = static_cast<FDstRefExpr&>(Node).Target)
				{
					Visit(*Target);
				}
				break;
			case EDstNodeKind::Argument:
				if (FDstExprPtr& Value = static_cast<FDstArgument&>(Node).Value)
				{
					Visit(*Value);
				}
				break;
			case EDstNodeKind::Meta:
				for (TUniquePtr<FDstMetaEntry>& Entry : static_cast<FDstMeta&>(Node).Entries)
				{
					if (Entry)
					{
						Visit(*Entry);
					}
				}
				break;
			case EDstNodeKind::MetaEntry:
				if (FDstExprPtr& Value = static_cast<FDstMetaEntry&>(Node).Value)
				{
					Visit(*Value);
				}
				break;
			default:
				break;
			}
		}

		void VisitSlot(FDstExprPtr& Slot, TFunctionRef<void(FDstExprPtr&)> Visit)
		{
			if (Slot)
			{
				Visit(Slot);
			}
		}

		void VisitMetaSlots(FDstMeta* Meta, TFunctionRef<void(FDstExprPtr&)> Visit)
		{
			if (!Meta)
			{
				return;
			}
			for (TUniquePtr<FDstMetaEntry>& Entry : Meta->Entries)
			{
				VisitSlot(Entry->Value, Visit);
			}
		}

		// Calls Visit on every expression slot an expression owns directly, so a slot can be replaced. Argument
		// names and metadata keys are not slots: they are never substituted or expanded.
		void ForEachExprSlot(FDstNode& Node, TFunctionRef<void(FDstExprPtr&)> Visit)
		{
			switch (Node.Kind)
			{
			case EDstNodeKind::MemberExpr:
				VisitSlot(static_cast<FDstMemberExpr&>(Node).Object, Visit);
				break;
			case EDstNodeKind::CallExpr:
			{
				FDstCallExpr& Call = static_cast<FDstCallExpr&>(Node);
				VisitSlot(Call.Callee, Visit);
				for (FDstArgumentPtr& Arg : Call.Args)
				{
					VisitSlot(Arg->Value, Visit);
				}
				VisitMetaSlots(Call.Meta.Get(), Visit);
				break;
			}
			case EDstNodeKind::ListExpr:
				for (FDstExprPtr& Item : static_cast<FDstListExpr&>(Node).Items)
				{
					VisitSlot(Item, Visit);
				}
				break;
			case EDstNodeKind::TupleExpr:
				for (FDstArgumentPtr& Item : static_cast<FDstTupleExpr&>(Node).Items)
				{
					VisitSlot(Item->Value, Visit);
				}
				break;
			case EDstNodeKind::UnaryExpr:
				VisitSlot(static_cast<FDstUnaryExpr&>(Node).Operand, Visit);
				break;
			case EDstNodeKind::BinaryExpr:
				VisitSlot(static_cast<FDstBinaryExpr&>(Node).Lhs, Visit);
				VisitSlot(static_cast<FDstBinaryExpr&>(Node).Rhs, Visit);
				break;
			case EDstNodeKind::ParenExpr:
				VisitSlot(static_cast<FDstParenExpr&>(Node).Inner, Visit);
				break;
			case EDstNodeKind::RefExpr:
				VisitSlot(static_cast<FDstRefExpr&>(Node).Target, Visit);
				break;
			default:
				break;
			}
		}

		// Every span a node and its descendants carry, set to Span. Applied to everything cloned from a template
		// body, so diagnostics about expanded code point at the use the author wrote.
		void RetargetSpans(FDstNode& Node, const FDstSpan& Span)
		{
			Node.Span = Span;
			switch (Node.Kind)
			{
			case EDstNodeKind::MemberExpr: static_cast<FDstMemberExpr&>(Node).MemberSpan = Span; break;
			case EDstNodeKind::CallExpr: static_cast<FDstCallExpr&>(Node).ArgsSpan = Span; break;
			case EDstNodeKind::BinaryExpr: static_cast<FDstBinaryExpr&>(Node).OpSpan = Span; break;
			case EDstNodeKind::Argument: static_cast<FDstArgument&>(Node).NameSpan = Span; break;
			case EDstNodeKind::MetaEntry: static_cast<FDstMetaEntry&>(Node).KeySpan = Span; break;
			default: break;
			}
			ForEachChildNode(Node, [&Span](FDstNode& Child) { RetargetSpans(Child, Span); });
		}

		// -- expansion --------------------------------------------------------------------------

		// The parameters of the template being instantiated and what they are bound to.
		struct FBinding
		{
			const FDstTemplateStmt* Def = nullptr;
			/** One per parameter: the argument written at the use, or the parameter's default. */
			TArray<const FDstExpr*> Values;
			/** True when the value is an argument (already expanded, with real positions), false for a default. */
			TArray<bool> FromCall;
			/** The use being replaced, in the file being expanded. */
			FDstSpan UseSpan;

			int32 IndexOf(const FString& Name) const
			{
				return Def->Params.IndexOfByPredicate([&Name](const TUniquePtr<FDstParam>& Param)
				{
					return Param->Name.Equals(Name, ESearchCase::CaseSensitive);
				});
			}
		};

		struct FExpander
		{
			FDstDiagnosticSink& Sink;
			TArray<FTemplateDef> Defs;
			// FString keys hash and compare without regard to case; the buckets only narrow the search, the
			// comparison that decides is case-sensitive (identifiers are).
			TMap<FString, TArray<int32>> DefsByName;
			/** The file being expanded: its path goes on diagnostics and its side tables get the origins. */
			FDstFile* File = nullptr;

			explicit FExpander(FDstDiagnosticSink& InSink) : Sink(InSink) {}

			// -- templates --

			const FTemplateDef* FindTemplate(const FString& Name) const
			{
				const TArray<int32>* Bucket = DefsByName.Find(Name);
				if (!Bucket)
				{
					return nullptr;
				}
				for (const int32 Index : *Bucket)
				{
					if (Defs[Index].Stmt->Name.Equals(Name, ESearchCase::CaseSensitive))
					{
						return &Defs[Index];
					}
				}
				return nullptr;
			}

			void Collect(const FDstFile& Source)
			{
				if (!Source.Document)
				{
					return;
				}
				for (const FDstStmtPtr& Member : Source.Document->Members)
				{
					const FDstTemplateStmt* Template = Member->As<FDstTemplateStmt>();
					if (!Template)
					{
						continue;
					}
					if (!Template->Body)
					{
						Sink.ErrorIn(TEXT("DST3106"), Source.Path, Template->NameSpan,
							FString::Printf(TEXT("Template '%s' has no body; write it as 'Template %s = expression;'."), *Template->Name, *Template->Name));
						continue;
					}
					if (const FTemplateDef* Existing = FindTemplate(Template->Name))
					{
						Sink.ErrorIn(TEXT("DST3105"), Source.Path, Template->NameSpan,
							FString::Printf(TEXT("Template '%s' is already defined in %s (line %d)."),
								*Template->Name, *Existing->File, Existing->Stmt->NameSpan.Line),
							TEXT("Rename one of them."));
						continue;
					}
					DefsByName.FindOrAdd(Template->Name).Add(Defs.Num());
					Defs.Add(FTemplateDef{ Template, Source.Path });
				}
			}

			// What an expression is a use of, if anything.
			//   `Name(args)`  a call whose callee is a bare name of a template
			//   `Name`        a bare name of a template declared without a parameter list
			// A name that is a parameter of the template being instantiated is never a use: the parameter wins.
			const FTemplateDef* FindUse(FDstExpr& Expr, const FBinding* Env, FDstCallExpr*& OutCall) const
			{
				OutCall = nullptr;
				if (FDstCallExpr* Call = Expr.As<FDstCallExpr>())
				{
					const FDstNameExpr* Callee = Call->Callee ? Call->Callee->As<FDstNameExpr>() : nullptr;
					if (!Callee || (Env && Env->IndexOf(Callee->Name) != INDEX_NONE))
					{
						return nullptr;
					}
					const FTemplateDef* Def = FindTemplate(Callee->Name);
					if (Def)
					{
						OutCall = Call;
					}
					return Def;
				}
				if (const FDstNameExpr* Name = Expr.As<FDstNameExpr>())
				{
					const FTemplateDef* Def = FindTemplate(Name->Name);
					return (Def && !Def->Stmt->bHasParams) ? Def : nullptr;
				}
				return nullptr;
			}

			// -- side tables --

			// Forgets a node and everything below it, so no table keeps an address that is about to be freed (and
			// then handed to a different node).
			void Purge(FDstNode& Node)
			{
				if (File->Origins.IsEmpty() && File->Trivia.IsEmpty())
				{
					return;
				}
				const FDstNode* Key = &Node;
				File->Origins.Remove(Key);
				File->Trivia.Remove(Key);
				ForEachChildNode(Node, [this](FDstNode& Child) { Purge(Child); });
			}

			// Replaces a slot's node. Comments kept for the old node (a list item has its own) go to the new one.
			void ReplaceSlot(FDstExprPtr& Slot, FDstExprPtr&& NewExpr)
			{
				const FDstNode* OldKey = Slot.Get();
				FDstTrivia Kept;
				const bool bHadTrivia = File->Trivia.RemoveAndCopyValue(OldKey, Kept);
				Purge(*Slot);
				Slot = MoveTemp(NewExpr);
				if (bHadTrivia)
				{
					const FDstNode* NewKey = Slot.Get();
					File->Trivia.Add(NewKey, MoveTemp(Kept));
				}
			}

			// Gives Copy (a clone of From) the origins From's tree has, node for node, so an expansion inside an
			// argument stays recorded after the argument is copied into the template body.
			void CopyOrigins(const FDstNode& From, FDstNode& To)
			{
				if (File->Origins.IsEmpty())
				{
					return;
				}
				if (const FDstExpansionOrigin* Found = File->Origins.Find(&From))
				{
					FDstExpansionOrigin Copy = *Found;
					const FDstNode* Key = &To;
					File->Origins.Add(Key, MoveTemp(Copy));
				}

				// ForEachChildNode wants mutable nodes; this walk only reads From.
				TArray<FDstNode*> FromChildren;
				TArray<FDstNode*> ToChildren;
				ForEachChildNode(const_cast<FDstNode&>(From), [&FromChildren](FDstNode& Child) { FromChildren.Add(&Child); });
				ForEachChildNode(To, [&ToChildren](FDstNode& Child) { ToChildren.Add(&Child); });
				if (FromChildren.Num() != ToChildren.Num())
				{
					return;
				}
				for (int32 Index = 0; Index < FromChildren.Num(); ++Index)
				{
					CopyOrigins(*FromChildren[Index], *ToChildren[Index]);
				}
			}

			// -- binding --

			// Binds the use's arguments to the template's parameters. False (with a diagnostic) when they do not fit.
			bool Bind(const FDstCallExpr* Call, const FDstTemplateStmt& Def, const FDstSpan& UseSpan, FBinding& OutBinding)
			{
				const int32 NumParams = Def.Params.Num();
				OutBinding.Def = &Def;
				OutBinding.UseSpan = UseSpan;
				OutBinding.Values.Init(nullptr, NumParams);
				OutBinding.FromCall.Init(false, NumParams);
				bool bOk = true;
				// An argument without a value is a parse error that has been reported; stay quiet about what follows from it.
				bool bBroken = false;

				int32 Positional = 0;
				if (Call)
				{
					for (const FDstArgumentPtr& Arg : Call->Args)
					{
						if (Arg->bOutput)
						{
							Sink.ErrorIn(TEXT("DST3107"), File->Path, Arg->Span,
								FString::Printf(TEXT("A template argument cannot be an output binding ('%s -> ...'); template '%s' only takes values."), *Arg->Name, *Def.Name),
								FString::Printf(TEXT("Write %s = value."), *Arg->Name));
							bOk = false;
							continue;
						}

						int32 Index = INDEX_NONE;
						if (Arg->IsNamed())
						{
							Index = Def.Params.IndexOfByPredicate([&Arg](const TUniquePtr<FDstParam>& Param)
							{
								return Param->Name.Equals(Arg->Name, ESearchCase::CaseSensitive);
							});
							if (Index == INDEX_NONE)
							{
								Sink.ErrorIn(TEXT("DST3102"), File->Path, Arg->NameSpan,
									FString::Printf(TEXT("Template '%s' has no parameter '%s'."), *Def.Name, *Arg->Name),
									NumParams > 0 ? FString::Printf(TEXT("Its parameters are: %s."), *DescribeParams(Def)) : FString(TEXT("It takes no arguments.")));
								bOk = false;
								continue;
							}
						}
						else
						{
							if (Positional >= NumParams)
							{
								Sink.ErrorIn(TEXT("DST3103"), File->Path, Arg->Span,
									FString::Printf(TEXT("Template '%s' takes %s; this argument is extra."), *Def.Name, *DescribeCount(NumParams)));
								bOk = false;
								++Positional;
								continue;
							}
							Index = Positional++;
						}

						if (!Arg->Value)
						{
							bBroken = true;
							continue;
						}
						if (OutBinding.Values[Index])
						{
							Sink.ErrorIn(TEXT("DST3102"), File->Path, Arg->IsNamed() ? Arg->NameSpan : Arg->Span,
								FString::Printf(TEXT("Parameter '%s' of template '%s' is given twice."), *Def.Params[Index]->Name, *Def.Name));
							bOk = false;
							continue;
						}
						OutBinding.Values[Index] = Arg->Value.Get();
						OutBinding.FromCall[Index] = true;
					}
				}
				if (bBroken)
				{
					return false;
				}

				for (int32 Index = 0; Index < NumParams; ++Index)
				{
					if (OutBinding.Values[Index])
					{
						continue;
					}
					if (Def.Params[Index]->Default)
					{
						OutBinding.Values[Index] = Def.Params[Index]->Default.Get();
					}
					else
					{
						Sink.ErrorIn(TEXT("DST3101"), File->Path, UseSpan,
							FString::Printf(TEXT("Template '%s' needs an argument for '%s'."), *Def.Name, *Def.Params[Index]->Name),
							FString::Printf(TEXT("Pass it positionally or as %s = value, or give the parameter a default in the template."), *Def.Params[Index]->Name));
						bOk = false;
					}
				}
				return bOk;
			}

			static FString DescribeCount(int32 Count)
			{
				if (Count == 0)
				{
					return TEXT("no arguments");
				}
				return Count == 1 ? FString(TEXT("1 argument")) : FString::Printf(TEXT("%d arguments"), Count);
			}

			static FString DescribeParams(const FDstTemplateStmt& Def)
			{
				TArray<FString> Names;
				for (const TUniquePtr<FDstParam>& Param : Def.Params)
				{
					Names.Add(Param->Name);
				}
				return FString::Join(Names, TEXT(", "));
			}

			// -- expansion --

			// Replaces a parameter name with its value. An argument was expanded where it was written and keeps its
			// own positions; a default belongs to the definition, so it takes the use's position and gets its own
			// template uses expanded here.
			void Substitute(FDstExprPtr& Slot, int32 Index, int32 Depth, const FBinding& Env)
			{
				FDstExprPtr Copy = CloneExpr(*Env.Values[Index]);
				if (Env.FromCall[Index])
				{
					CopyOrigins(*Env.Values[Index], *Copy);
				}
				else
				{
					RetargetSpans(*Copy, Env.UseSpan);
					ExpandSlot(Copy, Depth, nullptr);
				}
				// The name being replaced is a node of the body clone made for this expansion: no table knows it.
				Slot = MoveTemp(Copy);
			}

			// Args and settings of a call, without its callee.
			void ExpandCallParts(FDstCallExpr& Call, int32 Depth, const FBinding* Env)
			{
				for (FDstArgumentPtr& Arg : Call.Args)
				{
					ExpandSlot(Arg->Value, Depth, Env);
				}
				ExpandMeta(Call.Meta.Get(), Depth, Env);
			}

			void ExpandMeta(FDstMeta* Meta, int32 Depth, const FBinding* Env)
			{
				if (!Meta)
				{
					return;
				}
				for (TUniquePtr<FDstMetaEntry>& Entry : Meta->Entries)
				{
					ExpandSlot(Entry->Value, Depth, Env);
				}
			}

			// Walks an expression in place: a parameter of Env becomes its value, a template use becomes the
			// template's body. Env is null outside a template body.
			void ExpandSlot(FDstExprPtr& Slot, int32 Depth, const FBinding* Env)
			{
				if (!Slot)
				{
					return;
				}

				if (Env)
				{
					if (const FDstNameExpr* Name = Slot->As<FDstNameExpr>())
					{
						const int32 Index = Env->IndexOf(Name->Name);
						if (Index != INDEX_NONE)
						{
							Substitute(Slot, Index, Depth, *Env);
							return;
						}
					}
				}

				FDstCallExpr* Call = nullptr;
				const FTemplateDef* Def = FindUse(*Slot, Env, Call);
				if (!Def)
				{
					ForEachExprSlot(*Slot, [this, Depth, Env](FDstExprPtr& Child) { ExpandSlot(Child, Depth, Env); });
					return;
				}
				ExpandUse(Slot, *Def, Call, Depth, Env);
			}

			void ExpandUse(FDstExprPtr& Slot, const FTemplateDef& Def, FDstCallExpr* Call, int32 Depth, const FBinding* Env)
			{
				const FDstTemplateStmt& Stmt = *Def.Stmt;
				if (Depth >= MaxExpansionDepth)
				{
					Sink.ErrorIn(TEXT("DST3104"), File->Path, Slot->Span,
						FString::Printf(TEXT("Template '%s' expands more than %d levels deep; does it use itself?"), *Stmt.Name, MaxExpansionDepth));
					return;
				}

				// Arguments first, in the scope of the use, so each is expanded once rather than once per mention in
				// the body. The callee is the template's name.
				if (Call)
				{
					ExpandCallParts(*Call, Depth, Env);
				}

				FBinding Binding;
				if (!Bind(Call, Stmt, Slot->Span, Binding))
				{
					return;
				}

				FDstExpansionOrigin Origin;
				Origin.TemplateName = Stmt.Name;
				Origin.CallSpan = Slot->Span;
				Origin.DefinitionSpan = Stmt.Span;
				Origin.DefinitionFile = Def.File;

				// Settings written after the use, `Pause() [ Name = "x" ]`, belong to whatever the body turns out to be.
				TUniquePtr<FDstMeta> UseSettings = Call ? MoveTemp(Call->Meta) : nullptr;

				// The body is built on the side: the arguments it copies live in the use that is replaced below.
				FDstExprPtr Body = CloneExpr(*Stmt.Body);
				RetargetSpans(*Body, Binding.UseSpan);
				ExpandSlot(Body, Depth + 1, &Binding);

				ReplaceSlot(Slot, MoveTemp(Body));
				if (UseSettings)
				{
					AttachSettings(Slot, MoveTemp(UseSettings), Stmt);
				}

				// When the body was itself a single template use, the inner expansion already put its own origin on
				// the root; the author wrote the outer use, so that is the one to keep.
				const FDstNode* RootKey = Slot.Get();
				File->Origins.Add(RootKey, MoveTemp(Origin));
			}

			// Puts the settings written at a template use onto the call the template expanded to; a setting of the
			// same key replaces the template's own.
			void AttachSettings(FDstExprPtr& Slot, TUniquePtr<FDstMeta>&& UseSettings, const FDstTemplateStmt& Stmt)
			{
				FDstCallExpr* Root = Slot->As<FDstCallExpr>();
				if (!Root)
				{
					Sink.ErrorIn(TEXT("DST3108"), File->Path, UseSettings->Span,
						FString::Printf(TEXT("Template '%s' does not expand to a call, so these settings have nothing to apply to."), *Stmt.Name),
						TEXT("Settings follow a node call. Put them inside the template, or make its body a call."));
					return;
				}
				if (!Root->Meta)
				{
					Root->Meta = MoveTemp(UseSettings);
					return;
				}
				for (TUniquePtr<FDstMetaEntry>& Entry : UseSettings->Entries)
				{
					const int32 Existing = Root->Meta->Entries.IndexOfByPredicate([&Entry](const TUniquePtr<FDstMetaEntry>& Other)
					{
						return Other->Key.Equals(Entry->Key, ESearchCase::CaseSensitive);
					});
					if (Existing != INDEX_NONE)
					{
						Root->Meta->Entries[Existing] = MoveTemp(Entry);
					}
					else
					{
						Root->Meta->Entries.Add(MoveTemp(Entry));
					}
				}
			}

			// -- statements --

			void ExpandSection(FDstSectionStmt& Section)
			{
				for (FDstStmtPtr& Entry : Section.Entries)
				{
					if (FDstAssignStmt* Assign = Entry->As<FDstAssignStmt>())
					{
						ExpandSlot(Assign->Value, 0, nullptr);
						ExpandMeta(Assign->Meta.Get(), 0, nullptr);
					}
					else if (FDstDeclStmt* Decl = Entry->As<FDstDeclStmt>())
					{
						ExpandSlot(Decl->Value, 0, nullptr);
						ExpandMeta(Decl->Meta.Get(), 0, nullptr);
					}
					// `Value;` entries are names (tag prefixes); regions carry nothing.
				}
			}

			void ExpandClause(FDstClauseStmt& Clause)
			{
				switch (Clause.ClauseKind)
				{
				case EDstClauseKind::On:
				case EDstClauseKind::If:
				case EDstClauseKind::Utility:
					ExpandSlot(Clause.Value, 0, nullptr);
					break;
				case EDstClauseKind::Pick:
					// A mode keyword, not a value.
					break;
				case EDstClauseKind::Link:
					// The target names a subtree or an asset, not a template; what is passed to it are values.
					if (FDstCallExpr* Target = Clause.Value ? Clause.Value->As<FDstCallExpr>() : nullptr)
					{
						ExpandCallParts(*Target, 0, nullptr);
					}
					break;
				case EDstClauseKind::Do:
					for (TUniquePtr<FDstTaskItem>& Task : Clause.Tasks)
					{
						ExpandSlot(Task->Node, 0, nullptr);
					}
					break;
				case EDstClauseKind::Transition:
					// Clause.Value is the target state, a name.
					ExpandSlot(Clause.Trigger, 0, nullptr);
					ExpandSlot(Clause.Condition, 0, nullptr);
					ExpandMeta(Clause.Meta.Get(), 0, nullptr);
					break;
				}
			}

			void ExpandState(FDstStateStmt& State)
			{
				for (TUniquePtr<FDstParam>& Param : State.Params)
				{
					ExpandSlot(Param->Default, 0, nullptr);
				}
				ExpandMeta(State.Meta.Get(), 0, nullptr);

				for (FDstStmtPtr& Item : State.Body)
				{
					if (FDstClauseStmt* Clause = Item->As<FDstClauseStmt>())
					{
						ExpandClause(*Clause);
					}
					else if (FDstStateStmt* Child = Item->As<FDstStateStmt>())
					{
						ExpandState(*Child);
					}
				}
			}

			// Template definitions stay as written: a use inside a body is expanded where the template is used.
			void ExpandFile(FDstFile& Target)
			{
				if (!Target.Document)
				{
					return;
				}
				File = &Target;
				for (FDstStmtPtr& Member : Target.Document->Members)
				{
					if (FDstSectionStmt* Section = Member->As<FDstSectionStmt>())
					{
						ExpandSection(*Section);
					}
					else if (FDstStateStmt* State = Member->As<FDstStateStmt>())
					{
						ExpandState(*State);
					}
				}
			}
		};
	}

	bool FDstFileSystemIncludeLoader::Load(const FString& FromFile, const FString& IncludePath, FString& OutResolvedPath, FString& OutText)
	{
		TArray<FString> Candidates;
		if (!FPaths::IsRelative(IncludePath))
		{
			Candidates.Add(IncludePath);
		}
		else
		{
			Candidates.Add(FPaths::Combine(FPaths::GetPath(FromFile), IncludePath));
			for (const FString& Root : Roots)
			{
				Candidates.Add(FPaths::Combine(Root, IncludePath));
			}
		}

		for (FString& Candidate : Candidates)
		{
			Candidate = FPaths::ConvertRelativePathToFull(Candidate);
			FPaths::NormalizeFilename(Candidate);
			FPaths::CollapseRelativeDirectories(Candidate);
			if (IFileManager::Get().FileExists(*Candidate) && FFileHelper::LoadFileToString(OutText, *Candidate))
			{
				// The spelling the disk has, so one file never resolves to two strings that differ in case.
				FString OnDisk = IFileManager::Get().GetFilenameOnDisk(*Candidate);
				FPaths::NormalizeFilename(OnDisk);
				OutResolvedPath = OnDisk.IsEmpty() ? Candidate : OnDisk;
				return true;
			}
		}
		return false;
	}

	bool ResolveDstIncludes(FDstCompilationUnit& Unit, IDstIncludeLoader& Loader, FDstDiagnosticSink& Sink)
	{
		if (!Unit.Main)
		{
			return true;
		}
		const int32 ErrorsBefore = Sink.NumErrors();
		FIncludeWalker Walker(Unit, Loader, Sink);
		Walker.Parsed.Add(Unit.Main->Path);
		Walker.Stack.Add(Unit.Main->Path);
		Walker.Walk(*Unit.Main);
		return Sink.NumErrors() == ErrorsBefore;
	}

	bool ExpandDstTemplates(FDstCompilationUnit& Unit, FDstDiagnosticSink& Sink)
	{
		if (!Unit.Main || !Unit.Main->Document)
		{
			return true;
		}
		const int32 ErrorsBefore = Sink.NumErrors();

		FExpander Expander(Sink);
		Expander.Collect(*Unit.Main);
		for (const TUniquePtr<FDstFile>& Library : Unit.Libraries)
		{
			Expander.Collect(*Library);
		}

		if (!Expander.Defs.IsEmpty())
		{
			Expander.ExpandFile(*Unit.Main);
			// A library's subtrees are copied into the trees that link them after this step, so the template uses
			// inside them have to be gone by then. Their origins go in the library's own table.
			for (const TUniquePtr<FDstFile>& Library : Unit.Libraries)
			{
				Expander.ExpandFile(*Library);
			}
		}
		return Sink.NumErrors() == ErrorsBefore;
	}
}
