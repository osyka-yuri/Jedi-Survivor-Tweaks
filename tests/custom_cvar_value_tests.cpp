#include "tweaks/custom_cvar_value.hpp"
#include "test_check.hpp"

void TestCustomCVarValue() {
    using jst::tweaks::NormalizeCustomCVarValue;

    Check(NormalizeCustomCVarValue("true") == "1" &&
              NormalizeCustomCVarValue("FALSE") == "0",
          "custom CVar booleans normalize to numeric text");
    Check(NormalizeCustomCVarValue(" 1.25e-3 ") == "1.25e-3" &&
              NormalizeCustomCVarValue("+42") == "+42",
          "custom CVar finite numeric syntax is preserved verbatim");
    Check(!NormalizeCustomCVarValue("12junk") &&
              !NormalizeCustomCVarValue("nan") &&
              !NormalizeCustomCVarValue("1e9999") &&
              !NormalizeCustomCVarValue(""),
          "custom CVar validation rejects partial and non-finite values");
}
