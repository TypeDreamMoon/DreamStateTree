// Copyright DreamMoon. All Rights Reserved.
//
// Constant values: literals of the text <-> the engine's export text of one property.
//
// Both directions go through a value in memory, never through hand-built text:
//
//   literal  --apply-->  a temporary container holding the property  --DstExportDefault-->  export text
//   export text  --ImportText-->  a temporary container  --build-->  literal
//
// DstExportDefault is the one function that turns "this property in this container" into export text. The extractor
// uses it for the values it reads out of an asset, so the text Lower produces from a literal and the text Extract
// produces from the same value in an asset agree character for character (FProperty::ExportText_InContainer with the
// value as its own delta, PPF_None: every value and every struct field written out).
//
// Two checks keep the directions honest:
//   - DstLiteralToExportText imports its own result again and exports it once more; the two texts must be equal
//     (a value that does not survive its export text is an error, not a silent change).
//   - DstExportTextToLiteral converts the literal it built back (LiteralReproducesText below) and compares with the
//     canonical export text of the imported value. A property value the text cannot express (a localized FText, a
//     fixed-size array, an instanced subobject) therefore comes back as null instead of as a literal that reads back
//     as something else; the decompiler reports it as DST8004.
//
// The language has no way to say "the default of this struct" apart from leaving fields out, so a struct literal
// starts from the struct's default value and sets the fields it names. FLinearColor and FColor start fully opaque,
// because `(0.8, 0.25, 0.2)` is meant as a visible colour.

#include "DstValues.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "GameplayTagContainer.h"
#include "Lang/DstAst.h"
#include "Lang/DstPrinter.h"
#include "Lang/DstToken.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "StateTreeDelegate.h"
#include "StateTreePropertyRef.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

#include <limits>

namespace UE::DreamStateTree::Compiler
{
	// ----------------------------------------------------------------------------------------------------------------
	// Numbers
	// ----------------------------------------------------------------------------------------------------------------

	FString FormatDstNumber(double Value)
	{
		if (FMath::IsNaN(Value))
		{
			return TEXT("nan");
		}
		if (!FMath::IsFinite(Value))
		{
			return Value < 0.0 ? TEXT("-inf") : TEXT("inf");
		}
		if (Value == 0.0)
		{
			// Also folds -0 into 0: the two are the same value to every consumer.
			return TEXT("0");
		}

		// Whole numbers print as integers while every digit is exact, so 100 is `100` and not `1e+02`.
		if (FMath::Abs(Value) < 1e15)
		{
			const int64 Whole = static_cast<int64>(Value);
			if (static_cast<double>(Whole) == Value)
			{
				return FString::Printf(TEXT("%lld"), Whole);
			}
		}

		// The first precision that reads back as the same double is the shortest spelling of it.
		for (int32 Precision = 1; Precision <= 17; ++Precision)
		{
			FString Text = FString::Printf(TEXT("%.*g"), Precision, Value);
			if (FCString::Atod(*Text) == Value)
			{
				return Text;
			}
		}
		return FString::Printf(TEXT("%.17g"), Value);
	}

	namespace ValuesPrivate
	{
		using namespace UE::DreamStateTree::Lang;

		/** The same, for a property that holds a float: `0.1` and not `0.10000000149011612`. */
		FString FormatFloatNumber(float Value)
		{
			if (!FMath::IsFinite(Value))
			{
				return FormatDstNumber(Value);
			}
			if (Value == 0.f)
			{
				return TEXT("0");
			}
			if (FMath::Abs(Value) < 1e15f)
			{
				const int64 Whole = static_cast<int64>(Value);
				if (static_cast<float>(Whole) == Value)
				{
					return FString::Printf(TEXT("%lld"), Whole);
				}
			}
			for (int32 Precision = 1; Precision <= 9; ++Precision)
			{
				FString Text = FString::Printf(TEXT("%.*g"), Precision, static_cast<double>(Value));
				if (static_cast<float>(FCString::Atod(*Text)) == Value)
				{
					return Text;
				}
			}
			return FString::Printf(TEXT("%.9g"), static_cast<double>(Value));
		}

		// ------------------------------------------------------------------------------------------------------------
		// Building syntax tree nodes
		// ------------------------------------------------------------------------------------------------------------

		FDstExprPtr MakeString(const FString& Text)
		{
			TUniquePtr<FDstLiteralExpr> Expr = MakeUnique<FDstLiteralExpr>();
			Expr->LiteralKind = EDstLiteralKind::String;
			Expr->Text = Text;
			return FDstExprPtr(MoveTemp(Expr));
		}

		FDstExprPtr MakeBool(const bool bValue)
		{
			TUniquePtr<FDstLiteralExpr> Expr = MakeUnique<FDstLiteralExpr>();
			Expr->LiteralKind = EDstLiteralKind::Bool;
			Expr->bBool = bValue;
			Expr->Text = bValue ? TEXT("true") : TEXT("false");
			return FDstExprPtr(MoveTemp(Expr));
		}

		/** A number from FormatDstNumber / FormatFloatNumber output: the sign becomes a unary minus (literals carry none). */
		FDstExprPtr MakeNumberFromText(const FString& Formatted)
		{
			if (Formatted == TEXT("nan") || Formatted == TEXT("inf") || Formatted == TEXT("-inf"))
			{
				// Not a number the lexer reads.
				return MakeString(Formatted);
			}
			const bool bNegative = Formatted.StartsWith(TEXT("-"));
			const FString Digits = bNegative ? Formatted.RightChop(1) : Formatted;

			TUniquePtr<FDstLiteralExpr> Literal = MakeUnique<FDstLiteralExpr>();
			Literal->LiteralKind = EDstLiteralKind::Number;
			Literal->Text = Digits;
			Literal->Number = FCString::Atod(*Digits);
			if (!bNegative)
			{
				return FDstExprPtr(MoveTemp(Literal));
			}
			TUniquePtr<FDstUnaryExpr> Negation = MakeUnique<FDstUnaryExpr>();
			Negation->Op = EDstUnaryOp::Negate;
			Negation->Operand = FDstExprPtr(MoveTemp(Literal));
			return FDstExprPtr(MoveTemp(Negation));
		}

		/** A name; between backticks when it is not a plain identifier. */
		FDstExprPtr MakeName(const FString& Name)
		{
			TUniquePtr<FDstNameExpr> Expr = MakeUnique<FDstNameExpr>();
			Expr->Name = Name;
			Expr->bQuoted = !IsDstPlainIdentifier(Name);
			return FDstExprPtr(MoveTemp(Expr));
		}

		FDstExprPtr MakeMember(FDstExprPtr Object, const FString& Member)
		{
			TUniquePtr<FDstMemberExpr> Expr = MakeUnique<FDstMemberExpr>();
			Expr->Object = MoveTemp(Object);
			Expr->Member = Member;
			Expr->bMemberQuoted = !IsDstPlainIdentifier(Member);
			return FDstExprPtr(MoveTemp(Expr));
		}

		FDstArgumentPtr MakeArgument(const FString& Name, FDstExprPtr Value)
		{
			FDstArgumentPtr Argument = MakeUnique<FDstArgument>();
			Argument->Name = Name;
			Argument->Value = MoveTemp(Value);
			return Argument;
		}

		// ------------------------------------------------------------------------------------------------------------
		// Reading syntax tree nodes
		// ------------------------------------------------------------------------------------------------------------

		const FDstExpr& Unwrap(const FDstExpr& Expr)
		{
			const FDstExpr* Current = &Expr;
			while (const FDstParenExpr* Paren = Current->As<FDstParenExpr>())
			{
				if (!Paren->Inner)
				{
					break;
				}
				Current = Paren->Inner.Get();
			}
			return *Current;
		}

		/** What was written, short enough for a message. */
		FString Describe(const FDstExpr& Expr)
		{
			FString Text = PrintDstExpr(Expr);
			Text.ReplaceInline(TEXT("\r"), TEXT(""));
			Text.ReplaceInline(TEXT("\n"), TEXT(" "));
			if (Text.Len() > 48)
			{
				Text = Text.Left(45) + TEXT("...");
			}
			return Text;
		}

		FString Expected(const FString& What, const FDstExpr& Expr)
		{
			return FString::Printf(TEXT("Expected %s, found `%s`."), *What, *Describe(Expr));
		}

		bool IsNone(const FDstExpr& Expr)
		{
			const FDstNameExpr* Name = Unwrap(Expr).As<FDstNameExpr>();
			return Name && !Name->bQuoted && Name->Name.Equals(TEXT("None"), ESearchCase::CaseSensitive);
		}

		bool TryGetNumber(const FDstExpr& InExpr, double& OutNumber)
		{
			const FDstExpr& Expr = Unwrap(InExpr);
			if (const FDstLiteralExpr* Literal = Expr.As<FDstLiteralExpr>())
			{
				if (Literal->LiteralKind != EDstLiteralKind::Number)
				{
					return false;
				}
				// The lexeme is authoritative: it is what the printer writes back.
				OutNumber = Literal->Text.IsEmpty() ? Literal->Number : FCString::Atod(*Literal->Text);
				return true;
			}
			if (const FDstUnaryExpr* Unary = Expr.As<FDstUnaryExpr>())
			{
				double Inner = 0.0;
				if (Unary->Op == EDstUnaryOp::Negate && Unary->Operand && TryGetNumber(*Unary->Operand, Inner))
				{
					OutNumber = -Inner;
					return true;
				}
				return false;
			}
			// Arithmetic of constants (`1 - 0.5`) is folded here as well, so a caller may pass the expression as it stands.
			if (const FDstBinaryExpr* Binary = Expr.As<FDstBinaryExpr>())
			{
				double Left = 0.0;
				double Right = 0.0;
				if (!Binary->Lhs || !Binary->Rhs || !TryGetNumber(*Binary->Lhs, Left) || !TryGetNumber(*Binary->Rhs, Right))
				{
					return false;
				}
				switch (Binary->Op)
				{
				case EDstBinaryOp::Add: OutNumber = Left + Right; return true;
				case EDstBinaryOp::Subtract: OutNumber = Left - Right; return true;
				case EDstBinaryOp::Multiply: OutNumber = Left * Right; return true;
				case EDstBinaryOp::Divide:
					if (Right == 0.0)
					{
						return false;
					}
					OutNumber = Left / Right;
					return true;
				default:
					return false;
				}
			}
			return false;
		}

		bool TryGetBool(const FDstExpr& InExpr, bool& OutValue)
		{
			const FDstExpr& Expr = Unwrap(InExpr);
			if (const FDstLiteralExpr* Literal = Expr.As<FDstLiteralExpr>())
			{
				if (Literal->LiteralKind != EDstLiteralKind::Bool)
				{
					return false;
				}
				OutValue = Literal->bBool || Literal->Text.Equals(TEXT("true"), ESearchCase::CaseSensitive);
				return true;
			}
			if (const FDstNameExpr* Name = Expr.As<FDstNameExpr>(); Name && !Name->bQuoted)
			{
				if (Name->Name.Equals(TEXT("true"), ESearchCase::CaseSensitive))
				{
					OutValue = true;
					return true;
				}
				if (Name->Name.Equals(TEXT("false"), ESearchCase::CaseSensitive))
				{
					OutValue = false;
					return true;
				}
			}
			return false;
		}

		bool TryGetString(const FDstExpr& InExpr, FString& OutText)
		{
			const FDstLiteralExpr* Literal = Unwrap(InExpr).As<FDstLiteralExpr>();
			if (Literal && Literal->LiteralKind == EDstLiteralKind::String)
			{
				OutText = Literal->Text;
				return true;
			}
			return false;
		}

		/** A bare or backticked name (not `true` / `false`, which are values). */
		bool TryGetName(const FDstExpr& InExpr, FString& OutName)
		{
			const FDstNameExpr* Name = Unwrap(InExpr).As<FDstNameExpr>();
			if (!Name)
			{
				return false;
			}
			OutName = Name->Name;
			return true;
		}

		// ------------------------------------------------------------------------------------------------------------
		// A property in a container of its own
		// ------------------------------------------------------------------------------------------------------------

		/**
		 * Memory laid out the way the property's owner would, big enough for the property, with only the property
		 * initialised. The property is reached through ContainerPtrToValuePtr like anywhere else, which is what lets
		 * DstExportDefault take (Property, Container) for values made here and for values read from an asset alike.
		 */
		class FTempContainer
		{
		public:
			explicit FTempContainer(const FProperty& InProperty)
				: Property(InProperty)
			{
				const int32 Size = FMath::Max(Property.GetOffset_ForInternal() + Property.GetSize(), 1);
				Memory = FMemory::Malloc(Size, FMath::Max<int32>(16, Property.GetMinAlignment()));
				FMemory::Memzero(Memory, Size);
				Property.InitializeValue_InContainer(Memory);
			}

			~FTempContainer()
			{
				Property.DestroyValue_InContainer(Memory);
				FMemory::Free(Memory);
			}

			FTempContainer(const FTempContainer&) = delete;
			FTempContainer& operator=(const FTempContainer&) = delete;

			void* Container() const { return Memory; }
			void* Value() const { return Property.ContainerPtrToValuePtr<void>(Memory); }

		private:
			const FProperty& Property;
			void* Memory = nullptr;
		};

		bool ImportInto(const FProperty& Property, const FTempContainer& Target, const FString& ExportText)
		{
			FOutputDeviceNull Null;
			return Property.ImportText_InContainer(*ExportText, Target.Container(), nullptr, PPF_None, &Null) != nullptr;
		}

		// ------------------------------------------------------------------------------------------------------------
		// Enums
		// ------------------------------------------------------------------------------------------------------------

		/** The real enumerators: UEnum counts the generated `_MAX` as one. */
		int32 GetEnumCount(const UEnum& Enum)
		{
			int32 Count = Enum.NumEnums();
			if (Count > 0 && Enum.GetNameStringByIndex(Count - 1).EndsWith(TEXT("_MAX"), ESearchCase::CaseSensitive))
			{
				--Count;
			}
			return Count;
		}

		bool FindEnumValue(const UEnum& Enum, const FString& Qualifier, const FString& ValueName, int64& OutValue, FString& OutError)
		{
			if (!Qualifier.IsEmpty())
			{
				const FString EnumName = Enum.GetName();
				const bool bQualifies = Qualifier.Equals(EnumName, ESearchCase::CaseSensitive)
					|| (EnumName.Len() > 1 && EnumName[0] == TEXT('E') && Qualifier.Equals(EnumName.RightChop(1), ESearchCase::CaseSensitive));
				if (!bQualifies)
				{
					OutError = FString::Printf(TEXT("`%s` is not %s; this property takes values of %s."), *Qualifier, *EnumName, *EnumName);
					return false;
				}
			}

			const int32 Count = GetEnumCount(Enum);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Enum.GetNameStringByIndex(Index).Equals(ValueName, ESearchCase::CaseSensitive))
				{
					OutValue = Enum.GetValueByIndex(Index);
					return true;
				}
			}
			// A user-defined enum names its values NewEnumerator0 and so on; the display name is what its author sees.
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Enum.GetDisplayNameTextByIndex(Index).ToString().Equals(ValueName, ESearchCase::CaseSensitive))
				{
					OutValue = Enum.GetValueByIndex(Index);
					return true;
				}
			}

			TArray<FString> Names;
			for (int32 Index = 0; Index < Count && Index < 8; ++Index)
			{
				Names.Add(Enum.GetNameStringByIndex(Index));
			}
			OutError = FString::Printf(TEXT("`%s` is not a value of %s. Values: %s%s."), *ValueName, *Enum.GetName(),
				*FString::Join(Names, TEXT(", ")), Count > 8 ? TEXT(", ...") : TEXT(""));
			return false;
		}

		FDstExprPtr MakeEnumExpr(const UEnum* Enum, const int64 Value)
		{
			if (!Enum)
			{
				return nullptr;
			}
			const int32 Index = Enum->GetIndexByValue(Value);
			if (Index == INDEX_NONE)
			{
				return nullptr;
			}
			const FString Name = Enum->GetNameStringByIndex(Index);
			if (Name.IsEmpty() || Name.Contains(TEXT("`")))
			{
				return nullptr;
			}
			return MakeName(Name);
		}

		// ------------------------------------------------------------------------------------------------------------
		// Gameplay tags
		// ------------------------------------------------------------------------------------------------------------

		bool TagExists(const FString& Name, FGameplayTag& OutTag)
		{
			OutTag = FGameplayTag::RequestGameplayTag(FName(*Name), false);
			return OutTag.IsValid();
		}

		/** Words that cannot start a tag path in the text: they would be read as a clause, an operator or a binding root. */
		bool IsReservedWord(const FString& Word)
		{
			static const TCHAR* const Reserved[] = {
				TEXT("Params"), TEXT("Event"), TEXT("None"), TEXT("true"), TEXT("false"), TEXT("on"), TEXT("if"), TEXT("pick"),
				TEXT("utility"), TEXT("link"), TEXT("asset"), TEXT("do"), TEXT("and"), TEXT("or"), TEXT("not"), TEXT("ref"),
				TEXT("State"), TEXT("Subtree"), TEXT("Template"),
			};
			for (const TCHAR* Candidate : Reserved)
			{
				if (Word.Equals(Candidate, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		FDstExprPtr MakeTagExpr(const FGameplayTag& Tag, const TArray<FString>& Prefixes);
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Tags (also for the semantic stage: `on Hit`, `[ Tag = X ]`, an event transition)
	// ----------------------------------------------------------------------------------------------------------------

	/**
	 * The tag a written name means: as written when the project has it, otherwise completed with the declared prefixes
	 * in turn. Several different completions are an error; so is none. An empty name or `None` is the empty tag.
	 */
	DREAMSTATETREECOMPILER_API bool ResolveDstTagName(const FString& Written, TConstArrayView<FString> Prefixes, FGameplayTag& OutTag, FString& OutError)
	{
		OutTag = FGameplayTag();
		if (Written.IsEmpty() || Written.Equals(TEXT("None"), ESearchCase::CaseSensitive))
		{
			return true;
		}
		if (ValuesPrivate::TagExists(Written, OutTag))
		{
			return true;
		}

		TArray<FGameplayTag> Completions;
		for (const FString& Prefix : Prefixes)
		{
			FGameplayTag Candidate;
			if (!Prefix.IsEmpty() && ValuesPrivate::TagExists(Prefix + TEXT(".") + Written, Candidate))
			{
				Completions.AddUnique(Candidate);
			}
		}
		if (Completions.Num() == 1)
		{
			OutTag = Completions[0];
			return true;
		}
		if (Completions.Num() > 1)
		{
			TArray<FString> Names;
			for (const FGameplayTag& Tag : Completions)
			{
				Names.Add(Tag.GetTagName().ToString());
			}
			OutError = FString::Printf(TEXT("The tag `%s` can mean %s. Write the full name."), *Written, *FString::Join(Names, TEXT(", ")));
			return false;
		}

		OutError = FString::Printf(TEXT("`%s` is not a gameplay tag of this project."), *Written);
		if (!Prefixes.IsEmpty())
		{
			OutError += FString::Printf(TEXT(" Tried it as written and after the prefixes %s."), *FString::Join(Prefixes, TEXT(", ")));
		}
		return false;
	}

	/**
	 * The shortest way to write Tag that ResolveDstTagName turns back into Tag: the full name with a declared prefix
	 * taken off, when what is left still means only this tag.
	 */
	DREAMSTATETREECOMPILER_API FString ShortenDstTagName(const FGameplayTag& Tag, TConstArrayView<FString> Prefixes)
	{
		const FString Full = Tag.GetTagName().ToString();
		FString Best = Full;
		for (const FString& Prefix : Prefixes)
		{
			const FString WithDot = Prefix + TEXT(".");
			if (Prefix.IsEmpty() || Full.Len() <= WithDot.Len() || !Full.StartsWith(WithDot, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString Short = Full.RightChop(WithDot.Len());
			if (Short.Len() >= Best.Len())
			{
				continue;
			}
			FGameplayTag Back;
			FString Ignored;
			if (ResolveDstTagName(Short, Prefixes, Back, Ignored) && Back == Tag)
			{
				Best = Short;
			}
		}
		return Best;
	}

	namespace ValuesPrivate
	{
		FDstExprPtr MakeTagExpr(const FGameplayTag& Tag, const TArray<FString>& Prefixes)
		{
			if (!Tag.IsValid())
			{
				return MakeName(TEXT("None"));
			}
			const FString Written = ShortenDstTagName(Tag, Prefixes);

			// A path of plain identifiers when every segment is one, else the string form every tag position accepts.
			TArray<FString> Segments;
			Written.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty*/ false);
			bool bPath = !Segments.IsEmpty() && !IsReservedWord(Segments[0]);
			for (const FString& Segment : Segments)
			{
				bPath = bPath && IsDstPlainIdentifier(Segment);
			}
			if (!bPath)
			{
				return MakeString(Written);
			}
			FDstExprPtr Path = MakeName(Segments[0]);
			for (int32 Index = 1; Index < Segments.Num(); ++Index)
			{
				Path = MakeMember(MoveTemp(Path), Segments[Index]);
			}
			return Path;
		}

		/** A tag from a path, a string, or `None`. */
		bool ApplyTag(FGameplayTag& OutTag, const FDstExpr& Expr, const FDstValueContext& Context, FString& OutError)
		{
			FString Written;
			if (IsNone(Expr))
			{
				OutTag = FGameplayTag();
				return true;
			}
			TArray<FString> Segments;
			if (TryGetString(Expr, Written))
			{
			}
			else if (TryGetDstPath(Unwrap(Expr), Segments))
			{
				Written = FString::Join(Segments, TEXT("."));
			}
			else
			{
				OutError = Expected(TEXT("a gameplay tag"), Expr);
				return false;
			}
			return ResolveDstTagName(Written, Context.TagPrefixes, OutTag, OutError);
		}
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Assets and classes
	// ----------------------------------------------------------------------------------------------------------------

	namespace ValuesPrivate
	{
		/**
		 * Top-level assets by name, from the asset registry. Built on first use and kept current from the registry's
		 * events, so a build that creates assets does not make the next lookup rescan everything.
		 */
		class FAssetNameIndex
		{
		public:
			static FAssetNameIndex& Get()
			{
				static FAssetNameIndex Instance;
				return Instance;
			}

			~FAssetNameIndex()
			{
				if (bHooked && FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
				{
					if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
					{
						AssetRegistry->OnAssetAdded().RemoveAll(this);
						AssetRegistry->OnAssetRemoved().RemoveAll(this);
						AssetRegistry->OnAssetRenamed().RemoveAll(this);
					}
				}
			}

			/** The assets called Name, split by whether the spelling matches exactly (asset names are not case-sensitive on disk). */
			void Find(const FString& Name, TArray<FAssetData>& OutExact, TArray<FAssetData>& OutOtherCase)
			{
				// Waited for before taking the lock: the registry reports what it finds through callbacks that take it.
				FModuleManager::Get().LoadModule(TEXT("AssetRegistry"));
				if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get(); AssetRegistry && AssetRegistry->IsLoadingAssets())
				{
					// A name that is not in the list yet would read as an unknown asset.
					AssetRegistry->WaitForCompletion();
				}

				FScopeLock ScopeLock(&Lock);
				EnsureBuilt();
				// FNAME_Find: a name nobody has made is no asset's name, and asking should not make it.
				const FName Key(*Name, FNAME_Find);
				if (Key.IsNone())
				{
					return;
				}
				if (const TArray<FAssetData>* List = ByName.Find(Key))
				{
					for (const FAssetData& Asset : *List)
					{
						(Asset.AssetName.ToString().Equals(Name, ESearchCase::CaseSensitive) ? OutExact : OutOtherCase).Add(Asset);
					}
				}
			}

		private:
			void EnsureBuilt()
			{
				if (bBuilt)
				{
					return;
				}
				IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
				if (!AssetRegistry)
				{
					return;
				}
				// Hooked before the scan so nothing added meanwhile is missed.
				AssetRegistry->OnAssetAdded().AddRaw(this, &FAssetNameIndex::OnAssetAdded);
				AssetRegistry->OnAssetRemoved().AddRaw(this, &FAssetNameIndex::OnAssetRemoved);
				AssetRegistry->OnAssetRenamed().AddRaw(this, &FAssetNameIndex::OnAssetRenamed);
				bHooked = true;

				AssetRegistry->EnumerateAllAssets([this](const FAssetData& Asset)
				{
					AddNoLock(Asset);
					return true;
				});
				bBuilt = true;
			}

			void AddNoLock(const FAssetData& Asset)
			{
				if (Asset.AssetName.IsNone() || Asset.IsRedirector() || !Asset.IsTopLevelAsset())
				{
					return;
				}
				TArray<FAssetData>& List = ByName.FindOrAdd(Asset.AssetName);
				const FSoftObjectPath Path = Asset.GetSoftObjectPath();
				List.RemoveAll([&Path](const FAssetData& Existing) { return Existing.GetSoftObjectPath() == Path; });
				List.Add(Asset);
			}

			void RemoveNoLock(const FSoftObjectPath& Path)
			{
				if (TArray<FAssetData>* List = ByName.Find(Path.GetAssetFName()))
				{
					List->RemoveAll([&Path](const FAssetData& Existing) { return Existing.GetSoftObjectPath() == Path; });
				}
			}

			void OnAssetAdded(const FAssetData& Asset)
			{
				FScopeLock ScopeLock(&Lock);
				if (bBuilt)
				{
					AddNoLock(Asset);
				}
			}

			void OnAssetRemoved(const FAssetData& Asset)
			{
				FScopeLock ScopeLock(&Lock);
				if (bBuilt)
				{
					RemoveNoLock(Asset.GetSoftObjectPath());
				}
			}

			void OnAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath)
			{
				FScopeLock ScopeLock(&Lock);
				if (bBuilt)
				{
					RemoveNoLock(FSoftObjectPath(OldObjectPath));
					AddNoLock(Asset);
				}
			}

			FCriticalSection Lock;
			TMap<FName, TArray<FAssetData>> ByName;
			bool bBuilt = false;
			bool bHooked = false;
		};

		enum class EAssetLookup : uint8 { Found, NotFound, Ambiguous };

		struct FAssetLookup
		{
			EAssetLookup Status = EAssetLookup::NotFound;
			FAssetData Asset;
			/** Ambiguous: every candidate. NotFound: assets with the name in another case. */
			TArray<FAssetData> Others;
		};

		bool IsAnyClass(const UClass* Class)
		{
			return !Class || Class == UObject::StaticClass();
		}

		/** The asset called Name whose class is RequiredClass or a child of it; found only when there is exactly one. */
		FAssetLookup LookupAsset(const FString& Name, const UClass* RequiredClass)
		{
			TArray<FAssetData> Exact;
			TArray<FAssetData> OtherCase;
			FAssetNameIndex::Get().Find(Name, Exact, OtherCase);

			const auto Accepts = [RequiredClass](const FAssetData& Asset)
			{
				if (IsAnyClass(RequiredClass))
				{
					return true;
				}
				const UClass* AssetClass = Asset.GetClass(EResolveClass::Yes);
				return AssetClass && AssetClass->IsChildOf(RequiredClass);
			};
			Exact.RemoveAll([&Accepts](const FAssetData& Asset) { return !Accepts(Asset); });
			OtherCase.RemoveAll([&Accepts](const FAssetData& Asset) { return !Accepts(Asset); });

			FAssetLookup Result;
			if (Exact.Num() == 1)
			{
				Result.Status = EAssetLookup::Found;
				Result.Asset = Exact[0];
			}
			else if (Exact.Num() > 1)
			{
				Result.Status = EAssetLookup::Ambiguous;
				Result.Others = MoveTemp(Exact);
			}
			else
			{
				Result.Others = MoveTemp(OtherCase);
			}
			return Result;
		}

		// ---- Path(Root, "Rel")

		/** `Path(Game, "AI/EQ_Wander")` -> `/Game/AI/EQ_Wander.EQ_Wander`. The root is Game, Engine or Plugin.<Name>. */
		bool TryGetAssetPathExpr(const FDstExpr& InExpr, FString& OutObjectPath)
		{
			const FDstCallExpr* Call = Unwrap(InExpr).As<FDstCallExpr>();
			if (!Call || !Call->Callee || Call->Args.Num() != 2 || !Call->Args[0] || !Call->Args[1] || !Call->Args[0]->Value || !Call->Args[1]->Value
				|| Call->Args[0]->IsNamed() || Call->Args[1]->IsNamed())
			{
				return false;
			}
			const FDstNameExpr* Callee = Unwrap(*Call->Callee).As<FDstNameExpr>();
			if (!Callee || !Callee->Name.Equals(TEXT("Path"), ESearchCase::CaseSensitive))
			{
				return false;
			}

			// Root: a name, a member chain (Plugin.DreamAnim) or the same written as a string.
			const FDstExpr& Root = Unwrap(*Call->Args[0]->Value);
			FString RootText;
			TArray<FString> RootSegments;
			if (!TryGetString(Root, RootText) && TryGetDstPath(Root, RootSegments))
			{
				RootText = FString::Join(RootSegments, TEXT("."));
			}

			FString Mount;
			if (RootText.Equals(TEXT("Game"), ESearchCase::CaseSensitive))
			{
				Mount = TEXT("/Game");
			}
			else if (RootText.Equals(TEXT("Engine"), ESearchCase::CaseSensitive))
			{
				Mount = TEXT("/Engine");
			}
			else if (RootText.StartsWith(TEXT("Plugin."), ESearchCase::CaseSensitive) && RootText.Len() > 7)
			{
				Mount = TEXT("/") + RootText.RightChop(7);
			}
			else
			{
				return false;
			}

			FString Rel;
			if (!TryGetString(*Call->Args[1]->Value, Rel))
			{
				return false;
			}
			Rel.TrimStartAndEndInline();
			while (Rel.StartsWith(TEXT("/")))
			{
				Rel.RightChopInline(1);
			}
			if (Rel.IsEmpty())
			{
				return false;
			}

			// A subobject is `Package:Sub`; the asset's `.Leaf` goes before the `:`.
			FString SubObject;
			int32 ColonIndex = INDEX_NONE;
			if (Rel.FindChar(TEXT(':'), ColonIndex))
			{
				SubObject = Rel.Mid(ColonIndex);
				Rel.LeftInline(ColonIndex);
			}

			FString Path = Mount + TEXT("/") + Rel;
			int32 SlashIndex = INDEX_NONE;
			Path.FindLastChar(TEXT('/'), SlashIndex);
			const FString Leaf = Path.Mid(SlashIndex + 1);
			if (!Leaf.Contains(TEXT(".")))
			{
				Path += TEXT(".") + Leaf;
			}
			OutObjectPath = Path + SubObject;
			return true;
		}

		/** The inverse: `/Game/AI/EQ_Wander.EQ_Wander` -> `Path(Game, "AI/EQ_Wander")`; null for a mount the form cannot name. */
		FDstExprPtr MakeAssetPathExpr(const FString& ObjectPath)
		{
			FString Main = ObjectPath;
			FString SubObject;
			int32 ColonIndex = INDEX_NONE;
			if (Main.FindChar(TEXT(':'), ColonIndex))
			{
				SubObject = Main.Mid(ColonIndex);
				Main.LeftInline(ColonIndex);
			}
			if (!Main.StartsWith(TEXT("/")))
			{
				return nullptr;
			}
			const int32 MountEnd = Main.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
			if (MountEnd == INDEX_NONE)
			{
				return nullptr;
			}
			const FString Mount = Main.Mid(1, MountEnd - 1);
			if (Mount.IsEmpty() || Mount == TEXT("Script") || Mount == TEXT("Temp") || Mount == TEXT("Memory"))
			{
				return nullptr;
			}

			// The object name repeats the package's last segment for every ordinary asset; leave it out then.
			FString Rel = Main.RightChop(MountEnd + 1);
			int32 DotIndex = INDEX_NONE;
			if (Rel.FindLastChar(TEXT('.'), DotIndex))
			{
				const FString PackagePart = Rel.Left(DotIndex);
				const FString ObjectName = Rel.Mid(DotIndex + 1);
				int32 SlashIndex = INDEX_NONE;
				PackagePart.FindLastChar(TEXT('/'), SlashIndex);
				if (ObjectName.Equals(PackagePart.Mid(SlashIndex + 1), ESearchCase::CaseSensitive))
				{
					Rel = PackagePart;
				}
			}
			Rel += SubObject;

			FDstExprPtr RootExpr;
			if (Mount == TEXT("Game") || Mount == TEXT("Engine"))
			{
				RootExpr = MakeName(Mount);
			}
			else if (IsDstPlainIdentifier(Mount))
			{
				RootExpr = MakeMember(MakeName(TEXT("Plugin")), Mount);
			}
			else
			{
				RootExpr = MakeString(TEXT("Plugin.") + Mount);
			}

			TUniquePtr<FDstCallExpr> Call = MakeUnique<FDstCallExpr>();
			Call->Callee = MakeName(TEXT("Path"));
			Call->Args.Add(MakeArgument(FString(), MoveTemp(RootExpr)));
			Call->Args.Add(MakeArgument(FString(), MakeString(Rel)));
			return FDstExprPtr(MoveTemp(Call));
		}

		/** An asset as the text writes it: its short name when that names it alone among the assets of RequiredClass, else Path(...). */
		FDstExprPtr MakeAssetRefExpr(const FString& ObjectPath, const UClass* RequiredClass)
		{
			const FSoftObjectPath SoftPath(ObjectPath);
			if (SoftPath.IsAsset())
			{
				const FString Name = SoftPath.GetAssetName();
				const FAssetLookup Lookup = LookupAsset(Name, RequiredClass);
				if (Lookup.Status == EAssetLookup::Found && Lookup.Asset.GetSoftObjectPath() == SoftPath && !Name.Contains(TEXT("`")))
				{
					return MakeName(Name);
				}
			}
			if (FDstExprPtr PathExpr = MakeAssetPathExpr(ObjectPath))
			{
				return PathExpr;
			}
			return MakeString(ObjectPath);
		}

		FString DescribeAssetLookupFailure(const TCHAR* Kind, const FString& Name, const UClass* RequiredClass, const FAssetLookup& Lookup)
		{
			if (Lookup.Status == EAssetLookup::Ambiguous)
			{
				TArray<FString> Paths;
				for (const FAssetData& Candidate : Lookup.Others)
				{
					Paths.Add(Candidate.GetObjectPathString());
				}
				FString Suggestion;
				if (FDstExprPtr PathExpr = MakeAssetPathExpr(Paths[0]))
				{
					Suggestion = FString::Printf(TEXT(" Write it as %s to choose."), *PrintDstExpr(*PathExpr));
				}
				return FString::Printf(TEXT("The %s name `%s` is not unique: %s.%s"), Kind, *Name, *FString::Join(Paths, TEXT(", ")), *Suggestion);
			}

			FString Message = FString::Printf(TEXT("No %s named `%s`"), Kind, *Name);
			if (!IsAnyClass(RequiredClass))
			{
				Message += FString::Printf(TEXT(" of type %s"), *RequiredClass->GetName());
			}
			Message += TEXT(" in the asset registry.");
			if (!Lookup.Others.IsEmpty())
			{
				Message += FString::Printf(TEXT(" Did you mean `%s`?"), *Lookup.Others[0].AssetName.ToString());
			}
			return Message;
		}

		// ---- classes

		/** A native class by name: `Actor`, `AActor` and `CombatEnemy` (for ACombatEnemy) all work. */
		UClass* FindNativeClass(const FString& Name)
		{
			if (Name.IsEmpty() || Name.Contains(TEXT("/")) || Name.Contains(TEXT(".")) || Name.Contains(TEXT(":")))
			{
				return nullptr;
			}
			const auto FindExact = [](const FString& Candidate) -> UClass*
			{
				// ExactClass leaves out Blueprint-generated classes (their class is UBlueprintGeneratedClass).
				UClass* Class = Cast<UClass>(StaticFindFirstObject(UClass::StaticClass(), *Candidate,
					EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::ExactClass, ELogVerbosity::NoLogging));
				return Class && Class->HasAnyClassFlags(CLASS_Native) ? Class : nullptr;
			};
			if (UClass* Class = FindExact(Name))
			{
				return Class;
			}
			// Reflection drops the C++ prefix letter: AActor is Actor.
			if (Name.Len() > 1 && FChar::IsUpper(Name[1]) && (Name[0] == TEXT('U') || Name[0] == TEXT('A') || Name[0] == TEXT('I')))
			{
				return FindExact(Name.RightChop(1));
			}
			return nullptr;
		}

		/** A class from an object path: a native class (`/Script/Engine.Actor`), a generated class, or a Blueprint asset. */
		UClass* LoadClassFromPath(const FString& ObjectPath)
		{
			const auto Load = [](const FString& Path) -> UObject*
			{
				if (UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *Path))
				{
					return Found;
				}
				if (Path.StartsWith(TEXT("/Script/")))
				{
					// Native objects are never loaded from disk.
					return nullptr;
				}
				return StaticLoadObject(UObject::StaticClass(), nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			};

			UObject* Object = Load(ObjectPath);
			if (!Object && !ObjectPath.EndsWith(TEXT("_C")))
			{
				Object = Load(ObjectPath + TEXT("_C"));
			}
			if (UClass* Class = Cast<UClass>(Object))
			{
				return Class;
			}
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
			{
				return Blueprint->GeneratedClass.Get();
			}
			return nullptr;
		}

		bool IsAcceptedClass(const UClass* Class, const UClass* MetaClass)
		{
			if (!MetaClass || Class->IsChildOf(MetaClass))
			{
				return true;
			}
			// A class property whose meta class is an interface takes the classes that implement it.
			return MetaClass->HasAnyClassFlags(CLASS_Interface) && Class->ImplementsInterface(MetaClass);
		}

		/** A class by the name the text writes: a native class first, then a Blueprint asset of that name. */
		bool FindClassByName(const FString& Name, const UClass* MetaClass, UClass*& OutClass, FString& OutError)
		{
			if (UClass* Native = FindNativeClass(Name))
			{
				if (!IsAcceptedClass(Native, MetaClass))
				{
					OutError = FString::Printf(TEXT("`%s` is not a %s."), *Name, *MetaClass->GetName());
					return false;
				}
				OutClass = Native;
				return true;
			}

			TArray<FAssetData> Exact;
			TArray<FAssetData> OtherCase;
			FAssetNameIndex::Get().Find(Name, Exact, OtherCase);
			Exact.RemoveAll([](const FAssetData& Asset) { return !Asset.IsInstanceOf(UBlueprint::StaticClass(), EResolveClass::Yes); });

			TArray<UClass*> Matches;
			for (const FAssetData& Asset : Exact)
			{
				if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset()))
				{
					if (UClass* Generated = Blueprint->GeneratedClass.Get(); Generated && IsAcceptedClass(Generated, MetaClass))
					{
						Matches.Add(Generated);
					}
				}
			}
			if (Matches.Num() == 1)
			{
				OutClass = Matches[0];
				return true;
			}
			if (Matches.Num() > 1)
			{
				TArray<FString> Paths;
				for (const UClass* Match : Matches)
				{
					Paths.Add(Match->GetPathName());
				}
				OutError = FString::Printf(TEXT("The class name `%s` is not unique: %s. Write it as Path(Root, \"Rel\") to choose."), *Name,
					*FString::Join(Paths, TEXT(", ")));
				return false;
			}
			OutError = FString::Printf(TEXT("No class named `%s`%s. Write a C++ class (`Actor`, `CombatEnemy`) or a Blueprint's asset name (`BP_Enemy`)."), *Name,
				MetaClass && MetaClass != UObject::StaticClass() ? *FString::Printf(TEXT(" that is a %s"), *MetaClass->GetName()) : TEXT(""));
			return false;
		}

		/** A class as the text writes it, from the object path of the class (`/Script/Engine.Actor`, `/Game/AI/BP_Enemy.BP_Enemy_C`). */
		FDstExprPtr MakeClassRefExpr(const FString& ClassPath)
		{
			if (ClassPath.IsEmpty() || ClassPath.Equals(TEXT("None"), ESearchCase::CaseSensitive))
			{
				return MakeName(TEXT("None"));
			}

			if (ClassPath.StartsWith(TEXT("/Script/")))
			{
				int32 DotIndex = INDEX_NONE;
				ClassPath.FindLastChar(TEXT('.'), DotIndex);
				const FString Name = ClassPath.Mid(DotIndex + 1);
				const UClass* Native = FindNativeClass(Name);
				if (Native && Native->GetPathName().Equals(ClassPath, ESearchCase::IgnoreCase) && !Name.Contains(TEXT("`")))
				{
					return MakeName(Name);
				}
				return MakeString(ClassPath);
			}

			// A generated class belongs to the Blueprint asset of the same path without `_C`.
			FString AssetPath = ClassPath;
			AssetPath.RemoveFromEnd(TEXT("_C"));
			const FSoftObjectPath SoftPath(AssetPath);
			const FString Name = SoftPath.GetAssetName();
			if (SoftPath.IsAsset() && !Name.Contains(TEXT("`")) && !FindNativeClass(Name))
			{
				const FAssetLookup Lookup = LookupAsset(Name, UBlueprint::StaticClass());
				if (Lookup.Status == EAssetLookup::Found && Lookup.Asset.GetSoftObjectPath() == SoftPath)
				{
					return MakeName(Name);
				}
			}
			if (FDstExprPtr PathExpr = MakeAssetPathExpr(AssetPath))
			{
				return PathExpr;
			}
			return MakeString(ClassPath);
		}

		/** The class a literal names: a name, `Path(...)`, or a string holding an object path. */
		bool ResolveClassExpr(const FDstExpr& Expr, const UClass* MetaClass, UClass*& OutClass, FString& OutError)
		{
			FString ObjectPath;
			FString Name;
			if (TryGetAssetPathExpr(Expr, ObjectPath))
			{
			}
			else if (TryGetString(Expr, ObjectPath) && ObjectPath.StartsWith(TEXT("/")))
			{
			}
			else if (TryGetName(Expr, Name) || TryGetString(Expr, Name))
			{
				ObjectPath.Reset();
			}
			else
			{
				OutError = Expected(TEXT("a class name or Path(Root, \"Rel\")"), Expr);
				return false;
			}

			if (!ObjectPath.IsEmpty())
			{
				UClass* Class = LoadClassFromPath(ObjectPath);
				if (!Class)
				{
					OutError = FString::Printf(TEXT("`%s` is not a class that can be loaded."), *ObjectPath);
					return false;
				}
				if (!IsAcceptedClass(Class, MetaClass))
				{
					OutError = FString::Printf(TEXT("%s is not a %s."), *Class->GetName(), *MetaClass->GetName());
					return false;
				}
				OutClass = Class;
				return true;
			}
			return FindClassByName(Name, MetaClass, OutClass, OutError);
		}

		/** The object path an asset literal names: `Path(...)`, a string path, or the short name of an asset (found by the registry). */
		bool ResolveAssetExpr(const FDstExpr& Expr, const UClass* RequiredClass, FString& OutObjectPath, FString& OutError)
		{
			if (TryGetAssetPathExpr(Expr, OutObjectPath))
			{
				return true;
			}
			FString Text;
			if (TryGetString(Expr, Text) && Text.StartsWith(TEXT("/")))
			{
				OutObjectPath = Text;
				return true;
			}
			if (!TryGetName(Expr, Text) && !TryGetString(Expr, Text))
			{
				OutError = Expected(TEXT("an asset name or Path(Root, \"Rel\")"), Expr);
				return false;
			}
			const FAssetLookup Lookup = LookupAsset(Text, RequiredClass);
			if (Lookup.Status != EAssetLookup::Found)
			{
				OutError = DescribeAssetLookupFailure(TEXT("asset"), Text, RequiredClass, Lookup);
				return false;
			}
			OutObjectPath = Lookup.Asset.GetObjectPathString();
			return true;
		}
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Assets, for the semantic stage (`link asset ST_Other`)
	// ----------------------------------------------------------------------------------------------------------------

	/** `Path(Root, "Rel")` as an object path. False when Expr is not that call. */
	DREAMSTATETREECOMPILER_API bool TryGetDstAssetPath(const Lang::FDstExpr& Expr, FString& OutObjectPath)
	{
		return ValuesPrivate::TryGetAssetPathExpr(Expr, OutObjectPath);
	}

	/** The unique asset called Name whose class is AssetClass (or a child; null for any). Sets OutError otherwise. */
	DREAMSTATETREECOMPILER_API bool FindDstAsset(const FString& Name, const UClass* AssetClass, FString& OutObjectPath, FString& OutError)
	{
		const ValuesPrivate::FAssetLookup Lookup = ValuesPrivate::LookupAsset(Name, AssetClass);
		if (Lookup.Status != ValuesPrivate::EAssetLookup::Found)
		{
			OutError = ValuesPrivate::DescribeAssetLookupFailure(TEXT("asset"), Name, AssetClass, Lookup);
			return false;
		}
		OutObjectPath = Lookup.Asset.GetObjectPathString();
		return true;
	}

	/** The asset as the decompiler writes it: its short name when unique among assets of AssetClass, else Path(Root, "Rel"). */
	DREAMSTATETREECOMPILER_API Lang::FDstExprPtr MakeDstAssetExpr(const FString& ObjectPath, const UClass* AssetClass)
	{
		return ValuesPrivate::MakeAssetRefExpr(ObjectPath, AssetClass);
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Literal -> value
	// ----------------------------------------------------------------------------------------------------------------

	namespace ValuesPrivate
	{
		bool ApplyValue(const FProperty& Property, void* Value, const FDstExpr& Expr, const FDstValueContext& Context, FString& OutError);

		/** Base struct's fields first, each struct's in declaration order: the order tuples read and print in. */
		TArray<const FProperty*> GetFieldsInDeclarationOrder(const UStruct& Struct)
		{
			TArray<const UStruct*> Chain;
			for (const UStruct* Current = &Struct; Current; Current = Current->GetSuperStruct())
			{
				Chain.Insert(Current, 0);
			}
			TArray<const FProperty*> Fields;
			for (const UStruct* Current : Chain)
			{
				for (TFieldIterator<FProperty> It(Current, EFieldIterationFlags::None); It; ++It)
				{
					Fields.Add(*It);
				}
			}
			return Fields;
		}

		/** What a struct starts from before a literal sets its fields. */
		void ApplyLiteralDefaults(const UScriptStruct& Struct, void* Memory)
		{
			if (&Struct == TBaseStructure<FLinearColor>::Get())
			{
				static_cast<FLinearColor*>(Memory)->A = 1.0f;
			}
			else if (&Struct == TBaseStructure<FColor>::Get())
			{
				static_cast<FColor*>(Memory)->A = 255;
			}
		}

		bool IsBindingOnlyStruct(const UScriptStruct& Struct)
		{
			return Struct.IsChildOf(FStateTreePropertyRef::StaticStruct())
				|| Struct.IsChildOf(FStateTreeDelegateDispatcher::StaticStruct())
				|| Struct.IsChildOf(FStateTreeDelegateListener::StaticStruct());
		}

		bool ApplyNumber(const FNumericProperty& Property, void* Value, const FDstExpr& Expr, FString& OutError)
		{
			if (Property.IsFloatingPoint())
			{
				double Number = 0.0;
				if (TryGetNumber(Expr, Number))
				{
					Property.SetFloatingPointPropertyValue(Value, Number);
					return true;
				}
				// A non-finite value is not a number the lexer reads; the decompiler writes it as a string.
				FString Text;
				if (TryGetString(Expr, Text))
				{
					if (Text.Equals(TEXT("nan"), ESearchCase::CaseSensitive))
					{
						Property.SetFloatingPointPropertyValue(Value, std::numeric_limits<double>::quiet_NaN());
						return true;
					}
					if (Text.Equals(TEXT("inf"), ESearchCase::CaseSensitive) || Text.Equals(TEXT("+inf"), ESearchCase::CaseSensitive))
					{
						Property.SetFloatingPointPropertyValue(Value, std::numeric_limits<double>::infinity());
						return true;
					}
					if (Text.Equals(TEXT("-inf"), ESearchCase::CaseSensitive))
					{
						Property.SetFloatingPointPropertyValue(Value, -std::numeric_limits<double>::infinity());
						return true;
					}
				}
				OutError = Expected(TEXT("a number"), Expr);
				return false;
			}

			int64 Integer = 0;
			bool bHaveInteger = TryGetDstInteger(Expr, Integer);
			if (!bHaveInteger)
			{
				// A uint64 above INT64_MAX has no int64 spelling; its digits are read directly.
				if (CastField<FUInt64Property>(&Property))
				{
					const FDstLiteralExpr* Literal = Unwrap(Expr).As<FDstLiteralExpr>();
					bool bOnlyDigits = Literal && Literal->LiteralKind == EDstLiteralKind::Number && !Literal->Text.IsEmpty();
					if (bOnlyDigits)
					{
						for (const TCHAR Digit : Literal->Text)
						{
							bOnlyDigits = bOnlyDigits && FChar::IsDigit(Digit);
						}
					}
					if (bOnlyDigits)
					{
						Property.SetIntPropertyValue(Value, static_cast<uint64>(FCString::Strtoui64(*Literal->Text, nullptr, 10)));
						return true;
					}
				}
				// `2 + 3` or `4.0` folded: a whole number still.
				double Number = 0.0;
				if (TryGetNumber(Expr, Number) && FMath::Abs(Number) < 9.0e15 && Number == FMath::FloorToDouble(Number))
				{
					Integer = static_cast<int64>(Number);
					bHaveInteger = true;
				}
			}
			if (!bHaveInteger)
			{
				OutError = Expected(TEXT("a whole number"), Expr);
				return false;
			}
			if (!Property.CanHoldValue(Integer))
			{
				OutError = FString::Printf(TEXT("%lld does not fit this %s property."), Integer, *Property.GetClass()->GetName());
				return false;
			}
			Property.SetIntPropertyValue(Value, Integer);
			return true;
		}

		bool ApplyEnum(const UEnum* Enum, const FNumericProperty& Underlying, void* Value, const FDstExpr& Expr, FString& OutError)
		{
			if (!Enum)
			{
				OutError = TEXT("The property has no enum.");
				return false;
			}

			FString Qualifier;
			FString ValueName;
			const FDstExpr& Inner = Unwrap(Expr);
			TArray<FString> Segments;
			if (TryGetName(Inner, ValueName) || TryGetString(Inner, ValueName))
			{
				// `EMode::Walk` between backticks (or in a string) names the enum too.
				const int32 Scope = ValueName.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				if (Scope != INDEX_NONE)
				{
					Qualifier = ValueName.Left(Scope);
					ValueName.RightChopInline(Scope + 2);
				}
			}
			else if (TryGetDstPath(Inner, Segments) && Segments.Num() >= 2)
			{
				// `EMode.Walk`.
				ValueName = Segments.Last();
				Qualifier = Segments[Segments.Num() - 2];
			}
			else
			{
				OutError = Expected(FString::Printf(TEXT("the name of a value of %s"), *Enum->GetName()), Expr);
				return false;
			}

			int64 EnumValue = 0;
			if (!FindEnumValue(*Enum, Qualifier, ValueName, EnumValue, OutError))
			{
				return false;
			}
			Underlying.SetIntPropertyValue(Value, EnumValue);
			return true;
		}

		bool ApplyClass(const FObjectPropertyBase& Property, const UClass* MetaClass, const bool bSoft, void* Value, const FDstExpr& Expr, FString& OutError)
		{
			if (IsNone(Expr))
			{
				if (bSoft)
				{
					*static_cast<FSoftObjectPtr*>(Value) = FSoftObjectPtr();
				}
				else
				{
					Property.SetObjectPropertyValue(Value, nullptr);
				}
				return true;
			}

			UClass* Class = nullptr;
			if (!ResolveClassExpr(Expr, MetaClass, Class, OutError))
			{
				return false;
			}
			if (bSoft)
			{
				*static_cast<FSoftObjectPtr*>(Value) = FSoftObjectPtr(FSoftObjectPath(Class));
			}
			else
			{
				Property.SetObjectPropertyValue(Value, Class);
			}
			return true;
		}

		bool ApplyObject(const FObjectPropertyBase& Property, void* Value, const FDstExpr& Expr, FString& OutError)
		{
			const bool bSoft = CastField<FSoftObjectProperty>(&Property) != nullptr;
			if (IsNone(Expr))
			{
				if (bSoft)
				{
					*static_cast<FSoftObjectPtr*>(Value) = FSoftObjectPtr();
				}
				else
				{
					Property.SetObjectPropertyValue(Value, nullptr);
				}
				return true;
			}

			const UClass* RequiredClass = Property.PropertyClass.Get();
			FString ObjectPath;
			if (!ResolveAssetExpr(Expr, RequiredClass, ObjectPath, OutError))
			{
				return false;
			}

			if (bSoft)
			{
				// A soft reference keeps the path; nothing is loaded for it.
				*static_cast<FSoftObjectPtr*>(Value) = FSoftObjectPtr(FSoftObjectPath(ObjectPath));
				return true;
			}

			UObject* Object = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
			if (!Object && !ObjectPath.StartsWith(TEXT("/Script/")))
			{
				Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			if (!Object)
			{
				OutError = FString::Printf(TEXT("`%s` cannot be loaded."), *ObjectPath);
				return false;
			}
			if (RequiredClass && !Object->IsA(RequiredClass))
			{
				OutError = FString::Printf(TEXT("`%s` is a %s, not a %s."), *ObjectPath, *Object->GetClass()->GetName(), *RequiredClass->GetName());
				return false;
			}
			Property.SetObjectPropertyValue(Value, Object);
			return true;
		}

		bool ApplyStruct(const FStructProperty& Property, void* Value, const FDstExpr& InExpr, const FDstValueContext& Context, FString& OutError)
		{
			const UScriptStruct* Struct = Property.Struct;
			if (!Struct)
			{
				OutError = TEXT("The property's struct type is unknown.");
				return false;
			}
			const FDstExpr& Expr = Unwrap(InExpr);

			if (Struct == FGameplayTag::StaticStruct())
			{
				return ApplyTag(*static_cast<FGameplayTag*>(Value), Expr, Context, OutError);
			}
			if (Struct == FGameplayTagContainer::StaticStruct())
			{
				FGameplayTagContainer& Container = *static_cast<FGameplayTagContainer*>(Value);
				Container.Reset();
				TArray<const FDstExpr*> Items;
				if (const FDstListExpr* List = Expr.As<FDstListExpr>())
				{
					for (const FDstExprPtr& Item : List->Items)
					{
						if (Item)
						{
							Items.Add(Item.Get());
						}
					}
				}
				else if (!IsNone(Expr))
				{
					// One tag is a container of one.
					Items.Add(&Expr);
				}
				for (const FDstExpr* Item : Items)
				{
					FGameplayTag Tag;
					if (!ApplyTag(Tag, *Item, Context, OutError))
					{
						return false;
					}
					if (Tag.IsValid())
					{
						Container.AddTag(Tag);
					}
				}
				return true;
			}
			if (Struct == FSoftObjectPath::StaticStruct())
			{
				FSoftObjectPath& Path = *static_cast<FSoftObjectPath*>(Value);
				if (IsNone(Expr))
				{
					Path = FSoftObjectPath();
					return true;
				}
				FString ObjectPath;
				if (!ResolveAssetExpr(Expr, nullptr, ObjectPath, OutError))
				{
					return false;
				}
				Path = FSoftObjectPath(ObjectPath);
				return true;
			}
			if (IsBindingOnlyStruct(*Struct))
			{
				OutError = FString::Printf(TEXT("A %s is bound to something (`ref Path`, a delegate path); it has no constant."), *Struct->GetName());
				return false;
			}

			// The engine's own text, for a struct a tuple cannot spell.
			if (FString EngineText; TryGetString(Expr, EngineText))
			{
				FOutputDeviceNull Null;
				if (Property.ImportText_Direct(*EngineText, Value, nullptr, PPF_None, &Null) != nullptr)
				{
					return true;
				}
				OutError = FString::Printf(TEXT("\"%s\" is not a %s."), *EngineText, *Struct->GetName());
				return false;
			}

			// A tuple: `(0, 3000)` in declaration order, `(Min = 0, Max = 3000)` by field. `(5)` is a one-item tuple.
			struct FItem
			{
				FString Name;
				const FDstExpr* Value = nullptr;
				bool bOutput = false;
			};
			TArray<FItem> Items;
			if (const FDstTupleExpr* Tuple = Expr.As<FDstTupleExpr>())
			{
				for (const FDstArgumentPtr& Argument : Tuple->Items)
				{
					if (Argument && Argument->Value)
					{
						FItem& Item = Items.AddDefaulted_GetRef();
						Item.Name = Argument->Name;
						Item.Value = Argument->Value.Get();
						Item.bOutput = Argument->bOutput;
					}
				}
			}
			else if (InExpr.Is<FDstParenExpr>())
			{
				// Unwrap() removed the parentheses of `(5)`; a struct takes it as one positional item.
				FItem& Item = Items.AddDefaulted_GetRef();
				Item.Value = &Expr;
			}
			else
			{
				OutError = Expected(FString::Printf(TEXT("a tuple for %s, such as (X = 1, Y = 2)"), *Struct->GetName()), Expr);
				return false;
			}

			// Start from the struct's default; only the named fields change.
			Struct->DestroyStruct(Value);
			Struct->InitializeStruct(Value);
			ApplyLiteralDefaults(*Struct, Value);

			const TArray<const FProperty*> Fields = GetFieldsInDeclarationOrder(*Struct);
			TArray<const FProperty*> PositionalFields;
			for (const FProperty* Field : Fields)
			{
				if (!Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					PositionalFields.Add(Field);
				}
			}

			int32 Positional = 0;
			TArray<const FProperty*> Seen;
			for (const FItem& Item : Items)
			{
				if (Item.bOutput)
				{
					OutError = TEXT("An output binding (`->`) cannot be part of a constant.");
					return false;
				}
				const FProperty* Field = nullptr;
				if (!Item.Name.IsEmpty())
				{
					for (const FProperty* Candidate : Fields)
					{
						if (Candidate->GetName().Equals(Item.Name, ESearchCase::CaseSensitive))
						{
							Field = Candidate;
							break;
						}
					}
					if (!Field)
					{
						TArray<FString> Names;
						for (const FProperty* Candidate : PositionalFields)
						{
							Names.Add(Candidate->GetName());
						}
						OutError = FString::Printf(TEXT("%s has no field `%s`. Fields: %s."), *Struct->GetName(), *Item.Name, *FString::Join(Names, TEXT(", ")));
						return false;
					}
				}
				else
				{
					if (!PositionalFields.IsValidIndex(Positional))
					{
						OutError = FString::Printf(TEXT("%s has %d fields; the tuple gives more values."), *Struct->GetName(), PositionalFields.Num());
						return false;
					}
					Field = PositionalFields[Positional++];
				}

				if (Seen.Contains(Field))
				{
					OutError = FString::Printf(TEXT("The field `%s` of %s is given twice."), *Field->GetName(), *Struct->GetName());
					return false;
				}
				Seen.Add(Field);
				if (Field->ArrayDim != 1)
				{
					OutError = FString::Printf(TEXT("The field `%s` is a fixed-size array; it cannot be written as a constant."), *Field->GetName());
					return false;
				}

				FString FieldError;
				if (!ApplyValue(*Field, Field->ContainerPtrToValuePtr<void>(Value), *Item.Value, Context, FieldError))
				{
					OutError = FString::Printf(TEXT("%s: %s"), *Field->GetName(), *FieldError);
					return false;
				}
			}
			return true;
		}

		bool ApplyValue(const FProperty& Property, void* Value, const FDstExpr& InExpr, const FDstValueContext& Context, FString& OutError)
		{
			const FDstExpr& Expr = Unwrap(InExpr);

			if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property))
			{
				bool bValue = false;
				if (!TryGetBool(Expr, bValue))
				{
					OutError = Expected(TEXT("true or false"), Expr);
					return false;
				}
				BoolProperty->SetPropertyValue(Value, bValue);
				return true;
			}

			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
			{
				return ApplyEnum(EnumProperty->GetEnum(), *EnumProperty->GetUnderlyingProperty(), Value, Expr, OutError);
			}
			if (const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property); ByteProperty && ByteProperty->Enum)
			{
				return ApplyEnum(ByteProperty->Enum, *ByteProperty, Value, Expr, OutError);
			}
			if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(&Property))
			{
				return ApplyNumber(*NumericProperty, Value, Expr, OutError);
			}

			if (const FNameProperty* NameProperty = CastField<FNameProperty>(&Property))
			{
				FString Text;
				if (!TryGetString(Expr, Text) && !TryGetName(Expr, Text))
				{
					OutError = Expected(TEXT("a name, \"text\""), Expr);
					return false;
				}
				if (Text.Len() >= NAME_SIZE)
				{
					OutError = TEXT("The name is too long.");
					return false;
				}
				NameProperty->SetPropertyValue(Value, Text.Equals(TEXT("None"), ESearchCase::CaseSensitive) ? FName() : FName(*Text));
				return true;
			}
			if (const FStrProperty* StringProperty = CastField<FStrProperty>(&Property))
			{
				FString Text;
				if (!TryGetString(Expr, Text))
				{
					OutError = Expected(TEXT("a string \"text\""), Expr);
					return false;
				}
				StringProperty->SetPropertyValue(Value, Text);
				return true;
			}
			if (const FTextProperty* TextProperty = CastField<FTextProperty>(&Property))
			{
				FString Text;
				if (!TryGetString(Expr, Text))
				{
					OutError = Expected(TEXT("a string \"text\""), Expr);
					return false;
				}
				TextProperty->SetPropertyValue(Value, Text.IsEmpty() ? FText::GetEmpty() : FText::FromString(Text));
				return true;
			}

			// The class properties derive from the object properties: ask for them first.
			if (const FClassProperty* ClassProperty = CastField<FClassProperty>(&Property))
			{
				return ApplyClass(*ClassProperty, ClassProperty->MetaClass.Get(), /*bSoft*/ false, Value, Expr, OutError);
			}
			if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(&Property))
			{
				return ApplyClass(*SoftClassProperty, SoftClassProperty->MetaClass.Get(), /*bSoft*/ true, Value, Expr, OutError);
			}
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
			{
				return ApplyObject(*ObjectProperty, Value, Expr, OutError);
			}

			if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
			{
				return ApplyStruct(*StructProperty, Value, InExpr, Context, OutError);
			}

			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
			{
				const FDstListExpr* List = Expr.As<FDstListExpr>();
				if (!List)
				{
					OutError = Expected(TEXT("a list [a, b, c]"), Expr);
					return false;
				}
				// The list is the whole array; elements start from their defaults.
				FScriptArrayHelper Helper(ArrayProperty, Value);
				Helper.EmptyAndAddValues(List->Items.Num());
				for (int32 Index = 0; Index < List->Items.Num(); ++Index)
				{
					FString ElementError;
					if (!List->Items[Index] || !ApplyValue(*ArrayProperty->Inner, Helper.GetRawPtr(Index), *List->Items[Index], Context, ElementError))
					{
						OutError = FString::Printf(TEXT("[%d]: %s"), Index, *ElementError);
						return false;
					}
				}
				return true;
			}
			if (const FSetProperty* SetProperty = CastField<FSetProperty>(&Property))
			{
				const FDstListExpr* List = Expr.As<FDstListExpr>();
				if (!List)
				{
					OutError = Expected(TEXT("a list [a, b, c]"), Expr);
					return false;
				}
				FScriptSetHelper Helper(SetProperty, Value);
				Helper.EmptyElements();
				for (int32 Index = 0; Index < List->Items.Num(); ++Index)
				{
					FTempContainer Element(*SetProperty->ElementProp);
					FString ElementError;
					if (!List->Items[Index] || !ApplyValue(*SetProperty->ElementProp, Element.Value(), *List->Items[Index], Context, ElementError))
					{
						OutError = FString::Printf(TEXT("[%d]: %s"), Index, *ElementError);
						return false;
					}
					Helper.AddElement(Element.Value());
				}
				return true;
			}

			OutError = FString::Printf(TEXT("A %s property cannot be given a constant in the text."), *Property.GetClass()->GetName());
			return false;
		}
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Value -> literal
	// ----------------------------------------------------------------------------------------------------------------

	namespace ValuesPrivate
	{
		FDstExprPtr BuildLiteral(const FProperty& Property, const void* Value, const FDstValueContext& Context);

		FDstExprPtr BuildStruct(const FStructProperty& Property, const void* Value, const FDstValueContext& Context)
		{
			const UScriptStruct* Struct = Property.Struct;
			if (!Struct)
			{
				return nullptr;
			}
			if (Struct == FGameplayTag::StaticStruct())
			{
				return MakeTagExpr(*static_cast<const FGameplayTag*>(Value), Context.TagPrefixes);
			}
			if (Struct == FGameplayTagContainer::StaticStruct())
			{
				TUniquePtr<FDstListExpr> List = MakeUnique<FDstListExpr>();
				for (const FGameplayTag& Tag : static_cast<const FGameplayTagContainer*>(Value)->GetGameplayTagArray())
				{
					List->Items.Add(MakeTagExpr(Tag, Context.TagPrefixes));
				}
				return FDstExprPtr(MoveTemp(List));
			}
			if (Struct == FSoftObjectPath::StaticStruct())
			{
				const FSoftObjectPath& Path = *static_cast<const FSoftObjectPath*>(Value);
				return Path.IsNull() ? MakeName(TEXT("None")) : MakeAssetRefExpr(Path.ToString(), nullptr);
			}
			if (IsBindingOnlyStruct(*Struct))
			{
				return nullptr;
			}

			// Named fields that differ from the struct's default. A value equal to the default in every field (it differs
			// from the property's own default, or the caller would not ask) names all of its fields instead.
			FStructOnScope Default(Struct);
			ApplyLiteralDefaults(*Struct, Default.GetStructMemory());
			const TArray<const FProperty*> Fields = GetFieldsInDeclarationOrder(*Struct);

			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				TUniquePtr<FDstTupleExpr> Tuple = MakeUnique<FDstTupleExpr>();
				for (const FProperty* Field : Fields)
				{
					if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
					{
						continue;
					}
					if (Field->ArrayDim != 1)
					{
						// A fixed-size array has no literal; fine as long as it is the default.
						for (int32 ArrayIndex = 0; ArrayIndex < Field->ArrayDim; ++ArrayIndex)
						{
							if (!Field->Identical(Field->ContainerPtrToValuePtr<void>(Value, ArrayIndex),
								Field->ContainerPtrToValuePtr<void>(Default.GetStructMemory(), ArrayIndex), PPF_None))
							{
								return nullptr;
							}
						}
						continue;
					}

					const void* FieldValue = Field->ContainerPtrToValuePtr<void>(Value);
					const void* FieldDefault = Field->ContainerPtrToValuePtr<void>(Default.GetStructMemory());
					if (Pass == 0 && Field->Identical(FieldValue, FieldDefault, PPF_None))
					{
						continue;
					}
					FDstExprPtr Item = BuildLiteral(*Field, FieldValue, Context);
					if (!Item)
					{
						return nullptr;
					}
					Tuple->Items.Add(MakeArgument(Field->GetName(), MoveTemp(Item)));
				}
				if (!Tuple->Items.IsEmpty() || Pass == 1)
				{
					return FDstExprPtr(MoveTemp(Tuple));
				}
			}
			return nullptr;
		}

		FDstExprPtr BuildLiteral(const FProperty& Property, const void* Value, const FDstValueContext& Context)
		{
			if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property))
			{
				return MakeBool(BoolProperty->GetPropertyValue(Value));
			}
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
			{
				return MakeEnumExpr(EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
			}
			if (const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property); ByteProperty && ByteProperty->Enum)
			{
				return MakeEnumExpr(ByteProperty->Enum, ByteProperty->GetSignedIntPropertyValue(Value));
			}
			if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(&Property))
			{
				if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(NumericProperty))
				{
					return MakeNumberFromText(FormatFloatNumber(FloatProperty->GetPropertyValue(Value)));
				}
				if (NumericProperty->IsFloatingPoint())
				{
					return MakeNumberFromText(FormatDstNumber(NumericProperty->GetFloatingPointPropertyValue(Value)));
				}
				if (CastField<FUInt64Property>(NumericProperty))
				{
					const uint64 Unsigned = NumericProperty->GetUnsignedIntPropertyValue(Value);
					if (Unsigned > static_cast<uint64>(MAX_int64))
					{
						return MakeNumberFromText(FString::Printf(TEXT("%llu"), Unsigned));
					}
					return MakeDstInteger(static_cast<int64>(Unsigned));
				}
				return MakeDstInteger(NumericProperty->GetSignedIntPropertyValue(Value));
			}

			if (const FNameProperty* NameProperty = CastField<FNameProperty>(&Property))
			{
				const FName Name = NameProperty->GetPropertyValue(Value);
				return Name.IsNone() ? MakeName(TEXT("None")) : MakeString(Name.ToString());
			}
			if (const FStrProperty* StringProperty = CastField<FStrProperty>(&Property))
			{
				return MakeString(StringProperty->GetPropertyValue(Value));
			}
			if (const FTextProperty* TextProperty = CastField<FTextProperty>(&Property))
			{
				return MakeString(TextProperty->GetPropertyValue(Value).ToString());
			}

			if (const FClassProperty* ClassProperty = CastField<FClassProperty>(&Property))
			{
				const UObject* Class = ClassProperty->GetObjectPropertyValue(Value);
				return Class ? MakeClassRefExpr(Class->GetPathName()) : MakeName(TEXT("None"));
			}
			if (CastField<FSoftClassProperty>(&Property))
			{
				return MakeClassRefExpr(static_cast<const FSoftObjectPtr*>(Value)->ToSoftObjectPath().ToString());
			}
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
			{
				FString Path;
				if (CastField<FSoftObjectProperty>(ObjectProperty))
				{
					Path = static_cast<const FSoftObjectPtr*>(Value)->ToSoftObjectPath().ToString();
				}
				else if (const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value))
				{
					Path = Object->GetPathName();
				}
				return Path.IsEmpty() ? MakeName(TEXT("None")) : MakeAssetRefExpr(Path, ObjectProperty->PropertyClass.Get());
			}

			if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
			{
				return BuildStruct(*StructProperty, Value, Context);
			}

			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
			{
				FScriptArrayHelper Helper(ArrayProperty, Value);
				TUniquePtr<FDstListExpr> List = MakeUnique<FDstListExpr>();
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					FDstExprPtr Item = BuildLiteral(*ArrayProperty->Inner, Helper.GetRawPtr(Index), Context);
					if (!Item)
					{
						return nullptr;
					}
					List->Items.Add(MoveTemp(Item));
				}
				return FDstExprPtr(MoveTemp(List));
			}
			if (const FSetProperty* SetProperty = CastField<FSetProperty>(&Property))
			{
				FScriptSetHelper Helper(SetProperty, Value);
				TUniquePtr<FDstListExpr> List = MakeUnique<FDstListExpr>();
				for (FScriptSetHelper::FIterator It = Helper.CreateIterator(); It; ++It)
				{
					FDstExprPtr Item = BuildLiteral(*SetProperty->ElementProp, Helper.GetElementPtr(It), Context);
					if (!Item)
					{
						return nullptr;
					}
					List->Items.Add(MoveTemp(Item));
				}
				return FDstExprPtr(MoveTemp(List));
			}

			// Maps, interfaces, delegates and the rest have no literal.
			return nullptr;
		}

		/**
		 * The consistency check of the decompiler's direction. Literal came from a value; Canonical is that value's
		 * export text (DstExportDefault). Converting the literal back has to give exactly Canonical, which is what makes
		 * `value -> text -> value` the identity: whatever the literal cannot say makes the check fail and the caller
		 * report the property as inexpressible.
		 */
		bool LiteralReproducesText(const FDstExpr& Literal, const FProperty& Property, const FDstValueContext& Context, const FString& Canonical)
		{
			FString Back;
			FString Ignored;
			return DstLiteralToExportText(Literal, Property, Context, Back, Ignored) && Back.Equals(Canonical, ESearchCase::CaseSensitive);
		}
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Public API
	// ----------------------------------------------------------------------------------------------------------------

	FString DstExportDefault(const FProperty& Property, const void* DefaultContainer)
	{
		// The value is its own delta. Without one the engine compares against zero and writes nothing for false, 0 or
		// the first enumerator, and skips a struct's zero fields; against itself it writes every value and every field.
		FString Text;
		if (DefaultContainer)
		{
			Property.ExportText_InContainer(0, Text, DefaultContainer, DefaultContainer, nullptr, PPF_None);
			return Text;
		}
		const ValuesPrivate::FTempContainer Fresh(Property);
		Property.ExportText_InContainer(0, Text, Fresh.Container(), Fresh.Container(), nullptr, PPF_None);
		return Text;
	}

	bool DstLiteralToExportText(const Lang::FDstExpr& Literal, const FProperty& Property, const FDstValueContext& Context, FString& OutText, FString& OutError)
	{
		OutText.Reset();
		OutError.Reset();
		if (Property.ArrayDim != 1)
		{
			OutError = FString::Printf(TEXT("`%s` is a fixed-size array; it cannot be given a constant in the text."), *Property.GetName());
			return false;
		}

		const ValuesPrivate::FTempContainer Container(Property);
		if (!ValuesPrivate::ApplyValue(Property, Container.Value(), Literal, Context, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("The value does not fit the property.");
			}
			return false;
		}
		OutText = DstExportDefault(Property, Container.Container());

		// The text has to read back as the value it came from.
		const ValuesPrivate::FTempContainer Check(Property);
		if (!ValuesPrivate::ImportInto(Property, Check, OutText)
			|| !DstExportDefault(Property, Check.Container()).Equals(OutText, ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(TEXT("The value exports as `%s`, which the engine does not read back as the same value."), *OutText);
			OutText.Reset();
			return false;
		}
		return true;
	}

	Lang::FDstExprPtr DstExportTextToLiteral(const FString& ExportText, const FProperty& Property, const FDstValueContext& Context)
	{
		if (Property.ArrayDim != 1)
		{
			return nullptr;
		}

		// Empty text is the zero value: the model leaves it empty, and some properties do not import "" (a bool).
		const ValuesPrivate::FTempContainer Imported(Property);
		if (!ExportText.IsEmpty() && !ValuesPrivate::ImportInto(Property, Imported, ExportText))
		{
			return nullptr;
		}
		Lang::FDstExprPtr Literal = ValuesPrivate::BuildLiteral(Property, Imported.Value(), Context);
		if (!Literal)
		{
			return nullptr;
		}
		// Compared with the export text of the imported value, not with ExportText: the engine may spell the same value differently.
		if (!ValuesPrivate::LiteralReproducesText(*Literal, Property, Context, DstExportDefault(Property, Imported.Container())))
		{
			return nullptr;
		}
		return Literal;
	}
}
