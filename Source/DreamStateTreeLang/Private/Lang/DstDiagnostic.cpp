// Copyright DreamMoon. All Rights Reserved.
//
// FDstDiagnosticSink. The wire form `DSTnnnn (line,col): message` is what logs and the corpus
// expectations compare, so it carries no hint: a reworded hint must not break a golden.

#include "Lang/DstDiagnostic.h"

namespace UE::DreamStateTree::Lang
{
	const TCHAR* LexToString(const EDstSeverity Severity)
	{
		// Lower case, the spelling the DreamShader and DreamFX diagnostics use, so tools that read
		// all three can match on one set of words.
		switch (Severity)
		{
		case EDstSeverity::Warning:
			return TEXT("warning");
		case EDstSeverity::Info:
			return TEXT("info");
		case EDstSeverity::Error:
		default:
			return TEXT("error");
		}
	}

	FDstDiagnosticSink::FDstDiagnosticSink(FString InFilePath)
		: FilePath(MoveTemp(InFilePath))
	{
	}

	bool FDstDiagnosticSink::Error(const TCHAR* Code, const FDstSpan& Span, FString Message, FString Hint)
	{
		Add(EDstSeverity::Error, Code, FilePath, Span, MoveTemp(Message), MoveTemp(Hint));
		return false;
	}

	void FDstDiagnosticSink::Warning(const TCHAR* Code, const FDstSpan& Span, FString Message, FString Hint)
	{
		Add(EDstSeverity::Warning, Code, FilePath, Span, MoveTemp(Message), MoveTemp(Hint));
	}

	void FDstDiagnosticSink::Info(const TCHAR* Code, const FDstSpan& Span, FString Message, FString Hint)
	{
		Add(EDstSeverity::Info, Code, FilePath, Span, MoveTemp(Message), MoveTemp(Hint));
	}

	bool FDstDiagnosticSink::ErrorIn(const TCHAR* Code, const FString& InFilePath, const FDstSpan& Span, FString Message, FString Hint)
	{
		Add(EDstSeverity::Error, Code, InFilePath, Span, MoveTemp(Message), MoveTemp(Hint));
		return false;
	}

	void FDstDiagnosticSink::WarningIn(const TCHAR* Code, const FString& InFilePath, const FDstSpan& Span, FString Message, FString Hint)
	{
		Add(EDstSeverity::Warning, Code, InFilePath, Span, MoveTemp(Message), MoveTemp(Hint));
	}

	void FDstDiagnosticSink::Append(FDstDiagnosticSink&& Other)
	{
		Diagnostics.Reserve(Diagnostics.Num() + Other.Diagnostics.Num());
		for (FDstDiagnostic& Diagnostic : Other.Diagnostics)
		{
			if (Diagnostic.Severity == EDstSeverity::Error)
			{
				++ErrorCount;
			}
			Diagnostics.Add(MoveTemp(Diagnostic));
		}

		// The other sink has handed its contents over; its counters must say so.
		Other.Diagnostics.Reset();
		Other.ErrorCount = 0;
	}

	const FDstDiagnostic* FDstDiagnosticSink::FirstError() const
	{
		for (const FDstDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Severity == EDstSeverity::Error)
			{
				return &Diagnostic;
			}
		}
		return nullptr;
	}

	FString FDstDiagnosticSink::ToWireString(const FDstDiagnostic& Diagnostic)
	{
		return FString::Printf(TEXT("%s (%d,%d): %s"), *Diagnostic.Code, Diagnostic.Span.Line, Diagnostic.Span.Column, *Diagnostic.Message);
	}

	void FDstDiagnosticSink::Add(const EDstSeverity Severity, const TCHAR* Code, const FString& InFilePath, const FDstSpan& Span, FString&& Message, FString&& Hint)
	{
		FDstDiagnostic Diagnostic;
		Diagnostic.Code = Code;
		Diagnostic.Severity = Severity;
		Diagnostic.Message = MoveTemp(Message);
		Diagnostic.Hint = MoveTemp(Hint);
		Diagnostic.Span = Span;
		Diagnostic.FilePath = InFilePath;

		if (Severity == EDstSeverity::Error)
		{
			++ErrorCount;
		}

		Diagnostics.Add(MoveTemp(Diagnostic));
	}
}
