# XBlox Widget Fence Test

This page exercises the markdown `xblox` fence renderer with a compact subset of `tests/xblox/language.xblox`.
Use the widget toolbar's **Run Chain** action to execute the web-only runtime. `stdout` and `log` block events should appear in the run log panel.

The fence below uses the wrapper form:

- `options` configures the markdown widget.
- `document` is the XBlox document payload.

```xblox
{
  "options": {
    "showToolbar": true,
    "showPalette": false,
    "showProps": false,
    "hasProps": true,
    "showRunLog": false,
    "hasRunLog": true,
    "showLog": true,
    "hasLog": true,
    "showHelp": true,
    "editable": false,
    "defaultExpandedDepth": 3,
    "autoHeight": true
  },
  "document": {
    "version": 1,
    "context": {
      "score": 7,
      "mode": "beta",
      "user": {
        "name": "kim"
      },
      "numbers": [1, 2, 3],
      "labels": {
        "a": "red",
        "b": "blue"
      },
      "single": "solo"
    },
    "roots": [
      {
        "kind": "stdout",
        "message": "xblox language showcase"
      },
      {
        "kind": "log",
        "level": "info",
        "message": "run log is enabled for this markdown widget"
      },
      {
        "kind": "stdout",
        "message": "[1] if / elseIf / else - sibling chain, first match wins"
      },
      {
        "kind": "if",
        "condition": "score >= 9",
        "consequent": [
          {
            "kind": "stdout",
            "message": "score=${score} -> grade A (unexpected)"
          }
        ]
      },
      {
        "kind": "elseIf",
        "condition": "score >= 6",
        "items": [
          {
            "kind": "stdout",
            "message": "score=${score} -> grade B (elseIf matched)"
          },
          {
            "kind": "log",
            "level": "info",
            "message": "grade branch resolved from score=${score}"
          }
        ]
      },
      {
        "kind": "else",
        "items": [
          {
            "kind": "stdout",
            "message": "score=${score} -> grade F (skipped)"
          }
        ]
      },
      {
        "kind": "stdout",
        "message": "[2] nested chain - fresh chain inside a consequent"
      },
      {
        "kind": "if",
        "condition": "score > 0",
        "consequent": [
          {
            "kind": "if",
            "condition": "user.name == \"alex\"",
            "consequent": [
              {
                "kind": "stdout",
                "message": "hello alex (unexpected)"
              }
            ]
          },
          {
            "kind": "else",
            "items": [
              {
                "kind": "stdout",
                "message": "hello ${user.name} (inner else ran)"
              }
            ]
          }
        ]
      },
      {
        "kind": "stdout",
        "message": "[3] switch / case / default - first match, no fallthrough"
      },
      {
        "kind": "switch",
        "variable": "mode",
        "items": [
          {
            "kind": "case",
            "comparator": "===",
            "expression": "\"alpha\"",
            "consequent": [
              {
                "kind": "stdout",
                "message": "mode=alpha (unexpected)"
              }
            ]
          },
          {
            "kind": "case",
            "comparator": "===",
            "expression": "\"beta\"",
            "consequent": [
              {
                "kind": "stdout",
                "message": "mode=${mode} -> beta case ran"
              }
            ]
          },
          {
            "kind": "switchDefault",
            "consequent": [
              {
                "kind": "stdout",
                "message": "mode=${mode} -> default (unexpected)"
              }
            ]
          }
        ]
      },
      {
        "kind": "stdout",
        "message": "[4] while + break - exits nearest loop only"
      },
      {
        "kind": "setVariable",
        "name": "n",
        "value": 0
      },
      {
        "kind": "while",
        "condition": "n < 10",
        "loopLimit": 20,
        "items": [
          {
            "kind": "setVariable",
            "name": "n",
            "expression": "n + 1"
          },
          {
            "kind": "log",
            "level": "debug",
            "message": "while loop incremented n=${n}"
          },
          {
            "kind": "stdout",
            "message": "while tick n=${n}"
          },
          {
            "kind": "if",
            "condition": "n == 3",
            "consequent": [
              {
                "kind": "stdout",
                "message": "n reached 3 -> break"
              },
              {
                "kind": "break"
              }
            ]
          }
        ]
      },
      {
        "kind": "stdout",
        "message": "[5] iterator - fan out arbitrary scope/context values"
      },
      {
        "kind": "iterator",
        "input": "numbers",
        "filter": ".",
        "items": [
          {
            "kind": "stdout",
            "message": "array item ${CURRENT_INDEX}: ${CURRENT}"
          },
          {
            "kind": "log",
            "level": "info",
            "message": "iterator emitted array item ${CURRENT_INDEX}"
          }
        ]
      },
      {
        "kind": "iterator",
        "input": "labels",
        "filter": ".",
        "items": [
          {
            "kind": "stdout",
            "message": "object entry ${CURRENT_INDEX}: ${CURRENT}"
          }
        ]
      },
      {
        "kind": "stdout",
        "message": "done."
      }
    ]
  }
}
```
