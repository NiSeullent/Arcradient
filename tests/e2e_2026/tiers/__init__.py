import sys
sys.dont_write_bytecode = True

import importlib
from ..harness.types_and_guards import TestCaseSpec

TestCase = TestCaseSpec

_TIER_MODULES: dict[int, tuple[str, ...]] = {
    1: ("tier1_f01_f11", "tier1_f12_f19"),
    2: ("tier2_f01_f11", "tier2_f12_f19"),
    3: ("tier3_pairwise",),
    4: ("tier4_scenarios",),
}


def _load_module_tests(mod_name: str) -> list[TestCase]:
    mod = importlib.import_module(f".{mod_name}", package=__name__)
    getter = getattr(mod, "get_tests", None)
    if callable(getter):
        return list(getter())
    tests = getattr(mod, "TESTS", None)
    if tests is not None:
        return list(tests)
    raise RuntimeError(f"Tier module {mod_name!r} exposes neither get_tests() nor TESTS")


def get_tier_tests(tier: int) -> list[TestCase]:
    if tier not in _TIER_MODULES:
        raise ValueError(f"Unsupported tier {tier!r}; expected one of {sorted(_TIER_MODULES)}")
    out: list[TestCase] = []
    for mod_name in _TIER_MODULES[tier]:
        out.extend(_load_module_tests(mod_name))
    return out


def get_all_tests() -> list[TestCase]:
    out: list[TestCase] = []
    for tier in (1, 2, 3, 4):
        out.extend(get_tier_tests(tier))
    return out


__all__ = [
    "TestCase",
    "TestCaseSpec",
    "get_tier_tests",
    "get_all_tests",
]
