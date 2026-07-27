// RUDE - RAGE <-> Unreal Development Environment
#include "RudeInvoke.h"

#include "RudeToolset.h"
#include "UObject/UnrealType.h"

namespace
{
	bool IsAgentCallable(const UFunction* Fn)
	{
#if WITH_EDITORONLY_DATA
		// The same marker the MCP layer keys off, so every surface exposes one identical set.
		return Fn->HasMetaData(TEXT("AICallable"));
#else
		return true;
#endif
	}
}

TArray<UFunction*> FRudeInvoke::CollectTools()
{
	TArray<UFunction*> Tools;
	for (TFieldIterator<UFunction> It(URudeToolset::StaticClass(),
		EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (IsAgentCallable(*It))
		{
			Tools.Add(*It);
		}
	}
	Tools.Sort([](const UFunction& A, const UFunction& B) { return A.GetName() < B.GetName(); });
	return Tools;
}

UFunction* FRudeInvoke::FindTool(const FString& Name)
{
	for (UFunction* Fn : CollectTools())
	{
		if (Fn->GetName().Equals(Name, ESearchCase::IgnoreCase))
		{
			return Fn;
		}
	}
	return nullptr;
}

TArray<FString> FRudeInvoke::ParamNames(const UFunction* Fn)
{
	TArray<FString> Out;
	for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			Out.Add(It->GetName());
		}
	}
	return Out;
}

FString FRudeInvoke::ToolTip(const UFunction* Fn)
{
#if WITH_EDITORONLY_DATA
	return Fn->GetToolTipText().ToString();
#else
	return FString();
#endif
}

bool FRudeInvoke::IsAllStrings(const UFunction* Fn, FString& OutWhy)
{
	bool bFoundReturn = false;
	for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (!CastField<FStrProperty>(*It))
		{
			OutWhy = FString::Printf(TEXT("parameter '%s' is %s, not FString"),
				*It->GetName(), *It->GetClass()->GetName());
			return false;
		}
		if (It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			bFoundReturn = true;
		}
	}
	if (!bFoundReturn)
	{
		OutWhy = TEXT("no FString return value");
		return false;
	}
	return true;
}

bool FRudeInvoke::Call(UFunction* Fn, const TArray<FString>& Values, FString& OutResult,
                       FString& OutError)
{
	OutResult.Reset();
	OutError.Reset();
	if (!Fn)
	{
		OutError = TEXT("no such tool");
		return false;
	}
	if (!IsAllStrings(Fn, OutError))
	{
		// Refusing beats invoking with a zeroed argument frame, which would look like a real run.
		OutError = FString::Printf(TEXT("'%s' cannot be driven generically: %s"),
			*Fn->GetName(), *OutError);
		return false;
	}

	// A UFunction parameter frame: every value must be constructed before use and destroyed after,
	// or FString parameters leak on the way in and free garbage on the way out.
	TArray<uint8> Frame;
	Frame.SetNumZeroed(Fn->ParmsSize);
	for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Frame.GetData());
	}

	FStrProperty* ReturnProp = nullptr;
	int32 ValueIdx = 0;
	for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		FStrProperty* Str = CastField<FStrProperty>(*It);
		if (It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			ReturnProp = Str;
			continue;
		}
		if (Str && Values.IsValidIndex(ValueIdx))
		{
			Str->SetPropertyValue_InContainer(Frame.GetData(), Values[ValueIdx]);
		}
		++ValueIdx;
	}

	// The tools are static UFUNCTIONs; UE routes those through the class default object.
	URudeToolset::StaticClass()->GetDefaultObject()->ProcessEvent(Fn, Frame.GetData());

	if (ReturnProp)
	{
		OutResult = ReturnProp->GetPropertyValue_InContainer(Frame.GetData());
	}
	for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Frame.GetData());
	}
	return true;
}

bool FRudeInvoke::ReportedFailure(const FString& Result)
{
	return Result.Contains(TEXT("\"ok\":false")) || Result.Contains(TEXT("\"ok\": false"));
}
