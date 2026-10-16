// Copyright DreamMoon. All Rights Reserved.
//
// The semantic stage, file level: the header (asset path, schema and its context overrides), the sections (Settings,
// Parameters, Globals, Tags, Colors) and the checks on libraries.

#include "DstLowerPrivate.h"

#include "DreamStateTreeExtension.h"
#include "Interfaces/IPluginManager.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorTypes.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeSchema.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace UE::DreamStateTree::Compiler::Lowering
{
	using namespace Lang;
	using namespace Model;
	using Model::FDstNode;

	namespace
	{
		/** The meta key that names the context a schema property sets. */
		const TCHAR* ContextMetaKey = TEXT("DreamStateTreeContext");

		bool IsMemberOf(const FString& Name, const TCHAR* const* Names, const int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Name.Equals(Names[Index], ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		const FProperty* FindSchemaProperty(const UClass* SchemaClass, const FString& Key)
		{
			if (const FProperty* Direct = SchemaClass->FindPropertyByName(FName(*Key)))
			{
				return Direct;
			}
			return nullptr;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Shared helpers
	// ------------------------------------------------------------------------------------------

	FDstLowerer::FDstLowerer(const FDstCompilationUnit& InUnit, FDstLowerResult& InOut, FDstDiagnosticSink& InSink, const FDstLowerOptions& InOptions)
		: Unit(InUnit)
		, Out(InOut)
		, Sink(InSink)
		, Options(InOptions)
	{
	}

	const FDstSourceText* FDstLowerer::GetCurrentSource() const
	{
		if (Sink.CurrentFile.IsEmpty())
		{
			return &Unit.MainSource;
		}
		for (const FDstSourceText& Library : Unit.LibrarySources)
		{
			if (Library.GetPath() == Sink.CurrentFile)
			{
				return &Library;
			}
		}
		return &Unit.MainSource;
	}

	FString FDstLowerer::SourceOf(const FDstSpan& Span) const
	{
		const FDstSourceText* Source = GetCurrentSource();
		return Source ? Source->Slice(Span) : FString();
	}

	void FDstLowerer::AddPlace(const FString& Key, const FDstSpan& Span)
	{
		// A library's code has no span in the main file: it is placed where the main file linked it (see LowerLinkClause).
		if (Sink.CurrentFile.IsEmpty())
		{
			Out.Places.Add(Key, Span);
		}
	}

	FString FDstLowerer::DescribeExpr(const FDstExpr& Expr) const
	{
		const FString Path = GetDstPathString(Expr);
		if (!Path.IsEmpty())
		{
			return Path;
		}
		const FString Text = SourceOf(Expr.Span);
		return Text.IsEmpty() ? FString(TEXT("the expression")) : Text.Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" "));
	}

	FString FDstLowerer::PathOf(const FString& ParentPath, const FString& Name) const
	{
		return ParentPath.IsEmpty() ? Name : ParentPath + TEXT("/") + Name;
	}

	// ------------------------------------------------------------------------------------------
	// Header
	// ------------------------------------------------------------------------------------------

	const FProperty* FDstLowerer::FindContextProperty(const FString& ContextName) const
	{
		if (!SchemaClass)
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(SchemaClass); It; ++It)
		{
			if (It->HasMetaData(ContextMetaKey) && It->GetMetaData(ContextMetaKey) == ContextName)
			{
				return *It;
			}
		}
		// What the engine's own schemas call their context classes.
		if (ContextName.Equals(TEXT("Actor"), ESearchCase::CaseSensitive))
		{
			return SchemaClass->FindPropertyByName(TEXT("ContextActorClass"));
		}
		if (ContextName.Equals(TEXT("AIController"), ESearchCase::CaseSensitive))
		{
			return SchemaClass->FindPropertyByName(TEXT("AIControllerClass"));
		}
		return nullptr;
	}

	bool FDstLowerer::ReadHeader(const FDstDocument& Doc)
	{
		const int32 ErrorsBefore = Sink.Real.NumErrors();
		AddPlace(TEXT(""), Doc.KindSpan);

		static const TCHAR* const HeaderArgs[] = { TEXT("Name"), TEXT("Root"), TEXT("Schema") };
		for (const FDstArgumentPtr& Argument : Doc.HeaderArgs)
		{
			if (Argument && !IsMemberOf(Argument->Name, HeaderArgs, UE_ARRAY_COUNT(HeaderArgs)))
			{
				Sink.Error(TEXT("DST4006"), Argument->IsNamed() ? Argument->NameSpan : Argument->Span,
					FString::Printf(TEXT("The StateTree header has no argument `%s`."), *Argument->Name),
					TEXT("The header takes Name, Root and Schema."));
			}
		}

		// Name and Root -> the asset path.
		const FString Name = Doc.GetHeaderString(TEXT("Name"));
		const FString Root = Doc.GetHeaderString(TEXT("Root"));
		const FDstArgument* RootArgument = Doc.FindHeaderArg(TEXT("Root"));
		const FDstSpan RootSpan = RootArgument ? RootArgument->Span : Doc.KindSpan;

		if (Name.IsEmpty())
		{
			Sink.Error(TEXT("DST4006"), Doc.KindSpan, TEXT("The StateTree header needs a Name: the asset's path below its root."),
				TEXT("Write StateTree(Name=\"AI/ST_Brawler\", Schema=AIComponent)."));
		}

		FString Mount = Options.DefaultRoot.IsEmpty() ? FString(TEXT("Game")) : Options.DefaultRoot;
		if (!Root.IsEmpty())
		{
			if (Root.Equals(TEXT("Game"), ESearchCase::CaseSensitive) || Root.Equals(TEXT("Engine"), ESearchCase::CaseSensitive))
			{
				Mount = Root;
			}
			else if (Root.StartsWith(TEXT("Plugin.")) && Root.Len() > 7)
			{
				const FString PluginName = Root.RightChop(7);
				if (IPluginManager::Get().FindPlugin(PluginName).IsValid())
				{
					Mount = PluginName;
				}
				else
				{
					Sink.Error(TEXT("DST4006"), RootSpan, FString::Printf(TEXT("There is no plugin `%s`."), *PluginName),
						TEXT("Root=\"Plugin.<Name>\" names an enabled plugin."));
				}
			}
			else
			{
				Sink.Error(TEXT("DST4006"), RootSpan, FString::Printf(TEXT("`%s` is not a root."), *Root),
					TEXT("A root is \"Game\", \"Engine\" or \"Plugin.<Name>\"."));
			}
		}

		if (!Name.IsEmpty())
		{
			FString Relative = Name;
			Relative.TrimStartAndEndInline();
			Relative.RemoveFromStart(TEXT("/"));
			Relative.RemoveFromEnd(TEXT("/"));
			int32 Slash = INDEX_NONE;
			Relative.FindLastChar(TEXT('/'), Slash);
			const FString AssetName = Relative.Mid(Slash + 1);
			if (AssetName.IsEmpty())
			{
				Sink.Error(TEXT("DST4006"), Doc.KindSpan, FString::Printf(TEXT("`%s` does not name an asset."), *Name));
			}
			else
			{
				Out.Tree.AssetPath = FString::Printf(TEXT("/%s/%s.%s"), *Mount, *Relative, *AssetName);
			}
		}

		// Schema.
		const FDstArgument* SchemaArgument = Doc.FindHeaderArg(TEXT("Schema"));
		if (!SchemaArgument || !SchemaArgument->Value)
		{
			Sink.Error(TEXT("DST4005"), Doc.KindSpan, TEXT("The StateTree header does not say which schema the tree uses."),
				TEXT("Write Schema=AIComponent (or Schema=Component, or your game's schema)."));
			return false;
		}
		ReadSchemaArgument(*SchemaArgument);
		return Sink.Real.NumErrors() == ErrorsBefore;
	}

	void FDstLowerer::ReadSchemaArgument(const FDstArgument& Argument)
	{
		const FDstExpr& Value = *Argument.Value;
		const FDstCallExpr* Call = Value.As<FDstCallExpr>();
		const FDstExpr& Callee = Call && Call->Callee ? *Call->Callee : Value;

		const FString Written = GetDstPathString(Callee);
		if (Written.IsEmpty())
		{
			Sink.Error(TEXT("DST4004"), Value.Span, TEXT("The schema is written as a name, such as AIComponent."));
			return;
		}

		FDstNameRegistry& Registry = FDstNameRegistry::Get();
		const TArray<UClass*> Classes = Registry.ResolveSchema(Written);
		if (Classes.IsEmpty())
		{
			TArray<UClass*> All;
			GetDerivedClasses(UStateTreeSchema::StaticClass(), All, true);
			TArray<FString> Names;
			for (const UClass* Class : All)
			{
				if (!Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					Names.Add(Registry.GetSchemaName(Class));
				}
			}
			FString Hint = MakeSuggestionHint(Written, Names);
			if (Hint.IsEmpty() && !Names.IsEmpty())
			{
				Names.Sort();
				Hint = FString::Printf(TEXT("The schemas are %s."), *FString::Join(Names, TEXT(", ")));
			}
			Sink.Error(TEXT("DST4004"), Callee.Span, FString::Printf(TEXT("There is no schema called `%s`."), *Written), Hint);
			return;
		}
		if (Classes.Num() > 1)
		{
			TArray<FString> Names;
			for (const UClass* Class : Classes)
			{
				Names.Add(Class->GetPathName());
			}
			Sink.Error(TEXT("DST4004"), Callee.Span, FString::Printf(TEXT("`%s` could be any of: %s."), *Written, *FString::Join(Names, TEXT(", "))),
				TEXT("Give the schema's meta name (DreamStateTree=\"...\") a distinct value."));
			return;
		}

		SchemaClass = Classes[0];
		Out.Tree.SchemaClass = SchemaClass->GetPathName();
		Namespaces = Registry.GetSchemaNamespaces(SchemaClass);
		BuildSchemaObject();

		if (!Call)
		{
			return;
		}

		// Context overrides: `Actor = CombatEnemy`.
		for (const FDstArgumentPtr& Override : Call->Args)
		{
			if (!Override || !Override->Value)
			{
				continue;
			}
			if (!Override->IsNamed() || Override->bOutput)
			{
				Sink.Error(TEXT("DST2005"), Override->Span, TEXT("Schema arguments are named: the context, then its type."),
					TEXT("Write Schema=AIComponent(Actor = CombatEnemy)."));
				continue;
			}

			const FProperty* Property = FindContextProperty(Override->Name);
			if (!Property)
			{
				Property = FindSchemaProperty(SchemaClass, Override->Name);
			}
			if (!Property)
			{
				TArray<FString> Keys;
				for (const FContext& Context : Contexts)
				{
					Keys.Add(Context.Name);
				}
				Sink.Error(TEXT("DST4019"), Override->NameSpan,
					FString::Printf(TEXT("The schema %s has no context or setting called `%s`."), *Registry.GetSchemaName(SchemaClass), *Override->Name),
					Keys.IsEmpty() ? FString() : FString::Printf(TEXT("Its contexts are %s."), *FString::Join(Keys, TEXT(", "))));
				continue;
			}

			FString Text;
			FString Error;
			if (!DstLiteralToExportText(*Override->Value, *Property, ValueContext, Text, Error))
			{
				Sink.Error(TEXT("DST4008"), Override->Value->Span, Error);
				continue;
			}
			Out.Tree.SchemaProperties.Add(Property->GetName(), Text);
			if (Schema.IsValid())
			{
				void* Container = Schema.Get();
				Property->ImportText_InContainer(*Text, Container, Schema.Get(), PPF_None);
			}

			// The context takes the new class.
			for (FContext& Context : Contexts)
			{
				if (FindContextProperty(Context.Name) == Property)
				{
					if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
					{
						if (const UClass* Class = Cast<UClass>(ObjectProperty->GetObjectPropertyValue_InContainer(Schema.Get())))
						{
							Context.Struct = Class;
						}
					}
				}
			}
		}
	}

	void FDstLowerer::BuildSchemaObject()
	{
		if (!SchemaClass)
		{
			return;
		}
		Schema = TStrongObjectPtr<UStateTreeSchema>(NewObject<UStateTreeSchema>(GetTransientPackage(), SchemaClass, NAME_None, RF_Transient));
		if (!Schema.IsValid())
		{
			return;
		}

		Contexts.Reset();
		for (const FStateTreeExternalDataDesc& Desc : Schema->GetContextDataDescs())
		{
			FContext& Context = Contexts.AddDefaulted_GetRef();
			Context.Name = Desc.Name.ToString();
			Context.Struct = Desc.Struct;
		}

		Extensions.Reset();
		for (const UDreamStateTreeExtension* Extension : UDreamStateTreeExtension::FindFor(*Schema))
		{
			Extensions.Add(Extension);
		}
	}

	// ------------------------------------------------------------------------------------------
	// Tag prefixes
	// ------------------------------------------------------------------------------------------

	void FDstLowerer::CollectTagPrefixes()
	{
		TArray<FString> Prefixes;

		auto CollectFrom = [this, &Prefixes](const FDstFile& File, const FString& FilePath)
		{
			if (!File.Document)
			{
				return;
			}
			const FString SavedFile = Sink.CurrentFile;
			Sink.CurrentFile = FilePath;
			for (const FDstStmtPtr& Member : File.Document->Members)
			{
				const FDstSectionStmt* Section = Member ? Member->As<FDstSectionStmt>() : nullptr;
				if (!Section || !Section->Name.Equals(TEXT("Tags"), ESearchCase::CaseSensitive))
				{
					continue;
				}
				for (const FDstStmtPtr& Entry : Section->Entries)
				{
					if (!Entry || Entry->Is<FDstRegionStmt>())
					{
						continue;
					}
					const FDstExprStmt* Item = Entry->As<FDstExprStmt>();
					FString Prefix;
					if (Item && Item->Expr)
					{
						if (const FDstLiteralExpr* Literal = Item->Expr->As<FDstLiteralExpr>())
						{
							if (Literal->LiteralKind == EDstLiteralKind::String)
							{
								Prefix = Literal->Text;
							}
						}
						else
						{
							Prefix = GetDstPathString(*Item->Expr);
						}
					}
					if (Prefix.IsEmpty())
					{
						Sink.Error(TEXT("DST2009"), Entry->Span, TEXT("A `Tags` entry is a tag prefix such as `Game.Event.Villager;`."));
						continue;
					}
					Prefixes.AddUnique(Prefix);
				}
			}
			Sink.CurrentFile = SavedFile;
		};

		if (Unit.Main)
		{
			CollectFrom(*Unit.Main, FString());
		}
		for (const TUniquePtr<FDstFile>& Library : Unit.Libraries)
		{
			if (Library)
			{
				CollectFrom(*Library, Library->Path);
			}
		}

		ValueContext.TagPrefixes = Prefixes;
		Out.TagPrefixes = Prefixes;
	}

	// ------------------------------------------------------------------------------------------
	// Libraries
	// ------------------------------------------------------------------------------------------

	void FDstLowerer::CheckLibraries()
	{
		auto CheckFile = [this](const FDstFile& File, const FString& FilePath)
		{
			if (!File.Document || File.Document->DocumentKind != EDstDocumentKind::StateTreeLibrary)
			{
				return;
			}
			const FString SavedFile = Sink.CurrentFile;
			Sink.CurrentFile = FilePath;
			for (const FDstStmtPtr& Member : File.Document->Members)
			{
				if (!Member)
				{
					continue;
				}
				const bool bAllowed = Member->Is<FDstRegionStmt>() || Member->Is<FDstTemplateStmt>()
					|| (Member->Is<FDstSectionStmt>() && Member->As<FDstSectionStmt>()->Name.Equals(TEXT("Tags"), ESearchCase::CaseSensitive))
					|| (Member->Is<FDstStateStmt>() && Member->As<FDstStateStmt>()->bSubtree);
				if (!bAllowed)
				{
					Sink.Error(TEXT("DST6013"), Member->Span, TEXT("A library holds only Tags, Template and Subtree."),
						TEXT("Move this into the StateTree file that uses it."));
				}
			}
			Sink.CurrentFile = SavedFile;
		};

		if (Unit.Main && bIsLibraryMain)
		{
			CheckFile(*Unit.Main, FString());
		}
		for (const TUniquePtr<FDstFile>& Library : Unit.Libraries)
		{
			if (Library)
			{
				CheckFile(*Library, Library->Path);
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Sections
	// ------------------------------------------------------------------------------------------

	void FDstLowerer::LowerSections(const FDstDocument& Doc)
	{
		static const TCHAR* const SectionNames[] = { TEXT("Settings"), TEXT("Parameters"), TEXT("Globals"), TEXT("Tags"), TEXT("Colors") };

		TArray<FString> Seen;
		const FDstSectionStmt* Parameters = nullptr;
		const FDstSectionStmt* Colors = nullptr;
		const FDstSectionStmt* Settings = nullptr;
		const FDstSectionStmt* Globals = nullptr;
		for (const FDstStmtPtr& Member : Doc.Members)
		{
			const FDstSectionStmt* Section = Member ? Member->As<FDstSectionStmt>() : nullptr;
			if (!Section)
			{
				continue;
			}
			if (!IsMemberOf(Section->Name, SectionNames, UE_ARRAY_COUNT(SectionNames)))
			{
				TArray<FString> Names;
				for (const TCHAR* Name : SectionNames)
				{
					Names.Add(Name);
				}
				Sink.Error(TEXT("DST4013"), Section->NameSpan, FString::Printf(TEXT("`%s` is not a section."), *Section->Name),
					FString::Printf(TEXT("The sections are %s."), *FString::Join(Names, TEXT(", "))));
				continue;
			}
			if (Seen.Contains(Section->Name))
			{
				Sink.Error(TEXT("DST4013"), Section->NameSpan, FString::Printf(TEXT("The section `%s` is written twice."), *Section->Name),
					TEXT("Each section is written once; put all its entries in one."));
				continue;
			}
			Seen.Add(Section->Name);

			if (Section->Name.Equals(TEXT("Parameters"), ESearchCase::CaseSensitive)) { Parameters = Section; }
			else if (Section->Name.Equals(TEXT("Colors"), ESearchCase::CaseSensitive)) { Colors = Section; }
			else if (Section->Name.Equals(TEXT("Settings"), ESearchCase::CaseSensitive)) { Settings = Section; }
			else if (Section->Name.Equals(TEXT("Globals"), ESearchCase::CaseSensitive)) { Globals = Section; }
		}

		// Order matters: parameters and colors are read by everything that follows.
		if (Parameters)
		{
			LowerParametersSection(*Parameters);
		}
		else
		{
			FString Error;
			TreeParams.Build(Error);
		}
		if (Colors)
		{
			LowerColorsSection(*Colors);
		}
		if (Settings)
		{
			LowerSettingsSection(*Settings);
		}
		if (Globals)
		{
			PreScanGlobals(*Globals);
		}
	}

	void FDstLowerer::LowerSettingsSection(const FDstSectionStmt& Section)
	{
		// Properties of the editor data that are not state: kept out of Settings.
		static const TCHAR* const NotSettings[] = {
			TEXT("Schema"), TEXT("EditorSchema"), TEXT("Extensions"), TEXT("RootParameters"), TEXT("RootParameterPropertyBag"),
			TEXT("RootParametersGuid"), TEXT("Evaluators"), TEXT("GlobalTasks"), TEXT("EditorBindings"), TEXT("Colors"),
			TEXT("SubTrees"), TEXT("Breakpoints"), TEXT("CompiledDispatchers"),
		};

		const UClass* EditorDataClass = UStateTreeEditorData::StaticClass();
		const UStateTreeEditorData* Defaults = GetDefault<UStateTreeEditorData>();

		for (const FDstStmtPtr& Entry : Section.Entries)
		{
			if (!Entry || Entry->Is<FDstRegionStmt>())
			{
				continue;
			}
			const FDstAssignStmt* Assign = Entry->As<FDstAssignStmt>();
			if (!Assign || !Assign->Value)
			{
				Sink.Error(TEXT("DST2009"), Entry->Span, TEXT("A `Settings` entry is `Key = Value;`."));
				continue;
			}

			const FProperty* Property = EditorDataClass->FindPropertyByName(FName(*Assign->Name));
			if (!Property || IsMemberOf(Assign->Name, NotSettings, UE_ARRAY_COUNT(NotSettings)) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				TArray<FString> Keys;
				for (TFieldIterator<FProperty> It(EditorDataClass); It; ++It)
				{
					if (It->HasAnyPropertyFlags(CPF_Edit) && !It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) && !IsMemberOf(It->GetName(), NotSettings, UE_ARRAY_COUNT(NotSettings)))
					{
						Keys.Add(It->GetName());
					}
				}
				Sink.Error(TEXT("DST4014"), Assign->NameSpan, FString::Printf(TEXT("`%s` is not a setting of the tree."), *Assign->Name),
					MakeSuggestionHint(Assign->Name, Keys));
				continue;
			}

			FString Text;
			FString Error;
			if (!DstLiteralToExportText(*Assign->Value, *Property, ValueContext, Text, Error))
			{
				Sink.Error(TEXT("DST4008"), Assign->Value->Span, Error);
				continue;
			}

			const FString DefaultText = DstExportDefault(*Property, Defaults);
			if (Assign->Name.Equals(TEXT("GlobalTasksCompletion"), ESearchCase::CaseSensitive) && Schema.IsValid() && !Schema->AllowTasksCompletion() && Text != DefaultText)
			{
				Sink.Error(TEXT("DST4015"), Assign->Span, TEXT("The schema does not let a tree change how global tasks complete."),
					TEXT("Leave GlobalTasksCompletion out: it is always Any."));
				continue;
			}
			if (Text != DefaultText)
			{
				Out.Tree.Settings.Add(Assign->Name, Text);
			}
		}
	}

	void FDstLowerer::ReadParameterMeta(const FDstMeta* Meta, TMap<FString, FString>& OutMeta)
	{
		if (!Meta)
		{
			return;
		}
		for (const TUniquePtr<FDstMetaEntry>& Entry : Meta->Entries)
		{
			if (!Entry)
			{
				continue;
			}
			FString Value = TEXT("true");
			if (Entry->Value)
			{
				if (const FDstLiteralExpr* Literal = Entry->Value->As<FDstLiteralExpr>())
				{
					Value = Literal->Text;
				}
				else
				{
					const FString Path = GetDstPathString(*Entry->Value);
					if (Path.IsEmpty())
					{
						Sink.Error(TEXT("DST2018"), Entry->Value->Span, TEXT("A parameter's metadata value is a string, a number, true/false or a name."));
						continue;
					}
					Value = Path;
				}
			}
			OutMeta.Add(Entry->Key, Value);
		}
	}

	bool FDstLowerer::LowerParameterList(const TArray<FParamDecl>& Decls, FParamSet& OutSet, const FString& PlaceKeyPrefix)
	{
		bool bOk = true;
		TArray<const FParamDecl*> Accepted;
		for (const FParamDecl& Decl : Decls)
		{
			FString Canonical;
			FString Error;
			if (!ParseParameterType(Decl.TypeName, Canonical, Error))
			{
				Sink.Error(TEXT("DST4009"), Decl.TypeSpan, Error,
					TEXT("Types are bool, int, int64, float, double, Name, String, Text, a struct (Vector), an enum, a class (Actor), Class<Actor> and Array<T>."));
				bOk = false;
				continue;
			}
			if (OutSet.Find(Decl.Name))
			{
				Sink.Error(TEXT("DST6015"), Decl.NameSpan, FString::Printf(TEXT("The parameter `%s` is declared twice."), *Decl.Name));
				bOk = false;
				continue;
			}

			FDstParameter Parameter;
			Parameter.Name = Decl.Name;
			Parameter.Type = Canonical;
			ReadParameterMeta(Decl.Meta, Parameter.Meta);
			OutSet.Params.Add(MoveTemp(Parameter));
			Accepted.Add(&Decl);
			AddPlace(PlaceKeyPrefix + Decl.Name, Decl.Span);
		}

		FString BuildError;
		if (!OutSet.Build(BuildError))
		{
			Sink.Error(TEXT("DST4009"), Decls.IsEmpty() ? FDstSpan() : Decls[0].Span, BuildError);
			return false;
		}

		// Defaults: the property bag now gives every parameter a property to import into.
		for (const FParamDecl* Decl : Accepted)
		{
			if (!Decl->Default)
			{
				continue;
			}
			const FProperty* Property = OutSet.FindProperty(Decl->Name);
			if (!Property)
			{
				continue;
			}
			FString Text;
			FString Error;
			if (!DstLiteralToExportText(*Decl->Default, *Property, ValueContext, Text, Error))
			{
				Sink.Error(TEXT("DST4008"), Decl->Default->Span, Error);
				bOk = false;
				continue;
			}
			// A value equal to the type's zero value is the same as no value.
			if (Text == DstExportDefault(*Property, OutSet.GetDefaultMemory()))
			{
				Text.Reset();
			}
			for (FDstParameter& Parameter : OutSet.Params)
			{
				if (Parameter.Name == Decl->Name)
				{
					Parameter.ValueText = Text;
				}
			}
		}
		return bOk;
	}

	void FDstLowerer::LowerParametersSection(const FDstSectionStmt& Section)
	{
		TArray<FParamDecl> Decls;
		for (const FDstStmtPtr& Entry : Section.Entries)
		{
			if (!Entry || Entry->Is<FDstRegionStmt>())
			{
				continue;
			}
			const FDstDeclStmt* Decl = Entry->As<FDstDeclStmt>();
			if (!Decl)
			{
				Sink.Error(TEXT("DST2009"), Entry->Span, TEXT("A `Parameters` entry is `Type Name [= Value];`, for example `float Courage = 0.5;`."));
				continue;
			}
			FParamDecl& Item = Decls.AddDefaulted_GetRef();
			Item.TypeName = Decl->TypeName;
			Item.TypeSpan = Decl->TypeSpan;
			Item.Name = Decl->Name;
			Item.NameSpan = Decl->NameSpan;
			Item.Default = Decl->Value.Get();
			Item.Meta = Decl->Meta.Get();
			Item.Span = Decl->Span;
		}

		if (!Decls.IsEmpty() && Schema.IsValid() && !Schema->AllowGlobalParameters())
		{
			Sink.Error(TEXT("DST4015"), Section.NameSpan, TEXT("The schema does not allow parameters on the tree."),
				TEXT("Remove the Parameters section, or use a schema that takes parameters."));
		}

		LowerParameterList(Decls, TreeParams, TEXT("Param:"));
		Out.Tree.Parameters = TreeParams.Params;
	}

	void FDstLowerer::LowerColorsSection(const FDstSectionStmt& Section)
	{
		const FProperty* ColorProperty = FStateTreeEditorColor::StaticStruct()->FindPropertyByName(TEXT("Color"));
		for (const FDstStmtPtr& Entry : Section.Entries)
		{
			if (!Entry || Entry->Is<FDstRegionStmt>())
			{
				continue;
			}
			const FDstAssignStmt* Assign = Entry->As<FDstAssignStmt>();
			if (!Assign || !Assign->Value)
			{
				Sink.Error(TEXT("DST2009"), Entry->Span, TEXT("A `Colors` entry is `Name = (R, G, B);`."));
				continue;
			}
			if (ColorNames.Contains(Assign->Name))
			{
				Sink.Error(TEXT("DST6015"), Assign->NameSpan, FString::Printf(TEXT("The color `%s` is declared twice."), *Assign->Name));
				continue;
			}
			FString Text;
			FString Error;
			if (!ColorProperty || !DstLiteralToExportText(*Assign->Value, *ColorProperty, ValueContext, Text, Error))
			{
				Sink.Error(TEXT("DST4008"), Assign->Value->Span, Error.IsEmpty() ? FString(TEXT("A color is a tuple (R, G, B) or (R, G, B, A).")) : Error);
				continue;
			}
			ColorNames.Add(Assign->Name);
			FDstColor Color;
			Color.Name = Assign->Name;
			Color.ColorText = Text;
			Out.Tree.Colors.Add(MoveTemp(Color));
		}

		// The engine keeps colors in a set, which has no order; the extractor sorts them like this, so the model does too.
		Out.Tree.Colors.StableSort([](const FDstColor& A, const FDstColor& B)
		{
			const int32 ByName = A.Name.Compare(B.Name, ESearchCase::CaseSensitive);
			return ByName != 0 ? ByName < 0 : A.ColorText.Compare(B.ColorText, ESearchCase::CaseSensitive) < 0;
		});
	}

	// ------------------------------------------------------------------------------------------
	// Globals
	// ------------------------------------------------------------------------------------------

	void FDstLowerer::PreScanGlobals(const FDstSectionStmt& Section)
	{
		TArray<FGlobalEntry> Evaluators;
		TArray<FGlobalEntry> Tasks;
		for (const FDstStmtPtr& Entry : Section.Entries)
		{
			if (!Entry || Entry->Is<FDstRegionStmt>())
			{
				continue;
			}
			const FDstAssignStmt* Assign = Entry->As<FDstAssignStmt>();
			if (!Assign || !Assign->Value)
			{
				Sink.Error(TEXT("DST2009"), Entry->Span, TEXT("A `Globals` entry is `Name = Node(...);`: global nodes are named."));
				continue;
			}

			const FDstExpr& Value = *Assign->Value;
			const FDstCallExpr* Call = Value.As<FDstCallExpr>();
			const FDstExpr& Callee = Call && Call->Callee ? *Call->Callee : Value;

			const FDstNodeType* Type = nullptr;
			bool bWrongKind = false;
			if (!ResolveNodeType(Callee, EDstNodeCategory::Evaluator, EDstNodeCategory::Task, Value.Span, Type, &bWrongKind))
			{
				if (bWrongKind)
				{
					Sink.Error(TEXT("DST6014"), Value.Span,
						FString::Printf(TEXT("`%s` is not a global task or an evaluator."), *DescribeExpr(Callee)),
						TEXT("Globals holds evaluators and tasks that run for the whole tree."));
				}
				continue;
			}

			bool bDuplicate = false;
			for (const FBindableNode& Existing : GlobalNodes)
			{
				bDuplicate |= Existing.Name.Equals(Assign->Name, ESearchCase::CaseSensitive);
			}
			for (const FGlobalEntry& Existing : Evaluators)
			{
				bDuplicate |= Existing.Stmt && Existing.Stmt->Name.Equals(Assign->Name, ESearchCase::CaseSensitive);
			}
			for (const FGlobalEntry& Existing : Tasks)
			{
				bDuplicate |= Existing.Stmt && Existing.Stmt->Name.Equals(Assign->Name, ESearchCase::CaseSensitive);
			}
			if (bDuplicate || Assign->Name.Equals(TEXT("Params"), ESearchCase::CaseSensitive) || Assign->Name.Equals(TEXT("Event"), ESearchCase::CaseSensitive))
			{
				Sink.Error(TEXT("DST6015"), Assign->NameSpan, FString::Printf(TEXT("The name `%s` is already taken."), *Assign->Name),
					TEXT("Global nodes need names that differ from each other, from `Params`, `Event` and from the schema's contexts."));
				continue;
			}
			for (const FContext& Context : Contexts)
			{
				if (Context.Name == Assign->Name)
				{
					Sink.Error(TEXT("DST6015"), Assign->NameSpan, FString::Printf(TEXT("`%s` is the name of a context of the schema."), *Assign->Name));
					bDuplicate = true;
				}
			}
			if (bDuplicate)
			{
				continue;
			}

			FGlobalEntry Item;
			Item.Stmt = Assign;
			Item.Type = Type;
			Item.Role = Type->Category == EDstNodeCategory::Evaluator ? EDstNodeRole::Evaluator : EDstNodeRole::GlobalTask;
			(Item.Role == EDstNodeRole::Evaluator ? Evaluators : Tasks).Add(Item);
		}

		// Evaluators come first in the editor, then global tasks: the order a node can read from.
		GlobalEntries.Reset();
		GlobalEntries.Append(Evaluators);
		GlobalEntries.Append(Tasks);
		GlobalNodes.Reset();
		for (int32 Index = 0; Index < GlobalEntries.Num(); ++Index)
		{
			GlobalEntries[Index].ModelIndex = Index;
			FBindableNode Node;
			Node.Name = GlobalEntries[Index].Stmt->Name;
			Node.Key = FString::Printf(TEXT("Global[%d]"), Index);
			Node.Kind = EDstSourceKind::Global;
			Node.bIsTask = GlobalEntries[Index].Role == EDstNodeRole::GlobalTask;
			AddPlace(Node.Key, GlobalEntries[Index].Stmt->Span);
			AddPlace(TEXT("Global:") + Node.Name, GlobalEntries[Index].Stmt->Span);
			GlobalNodes.Add(MoveTemp(Node));
		}
	}

	void FDstLowerer::LowerGlobalsSection()
	{
		if (GlobalEntries.IsEmpty())
		{
			return;
		}
		if (Schema.IsValid() && !Schema->AllowEvaluators())
		{
			for (const FGlobalEntry& Entry : GlobalEntries)
			{
				if (Entry.Role == EDstNodeRole::Evaluator)
				{
					Sink.Error(TEXT("DST4015"), Entry.Stmt->Span, TEXT("The schema does not allow evaluators."),
						TEXT("Use a global task instead, or a schema that allows evaluators."));
				}
			}
		}

		for (const FGlobalEntry& Entry : GlobalEntries)
		{
			FBindScope Scope;
			Scope.Target = EBindTarget::Global;
			Scope.GlobalLimit = Entry.ModelIndex;

			FDstNode Node;
			FNodeInfoPtr Info;
			const FString Key = GlobalNodes[Entry.ModelIndex].Key;
			const EDstNodeCategory Category = Entry.Type->Category;
			if (LowerNode(*Entry.Stmt->Value, Entry.Stmt->Name, Entry.Stmt->NameSpan, Key, Entry.Role, Category, Scope, Node, &Info, Entry.Stmt->Meta.Get()))
			{
				GlobalNodes[Entry.ModelIndex].InstanceStruct = Info.IsValid() ? Info->InstanceStruct : nullptr;
				GlobalNodes[Entry.ModelIndex].bLowered = true;
			}
			Out.Tree.Globals.Add(MoveTemp(Node));
		}
	}
}
