import sys
from functools import lru_cache
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import pytest  # noqa: E402

from ecgcard import siggen  # noqa: E402
import make_fixtures  # noqa: E402


@lru_cache(maxsize=None)
def _sim(name: str) -> siggen.Simulation:
    return siggen.simulate(*make_fixtures._set()[name])


@pytest.fixture(scope="session")
def sim():
    """Callable returning the (cached) Simulation for a standard fixture name."""
    return _sim
