#!/usr/bin/env python3
"""The committed src/patches/menu_opt_handler.inc must be what tools/menu/lua/optHandler.lua compiles to.
  python -I test_menu_inc.py      (exit 0 = matches; otherwise run gen_menu_inc.py and commit the result)"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_menu_inc
sys.exit(gen_menu_inc.main(["--check"]))
