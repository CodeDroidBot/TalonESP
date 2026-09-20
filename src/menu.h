#pragma once
#include <Arduino.h>
#include <vector>

class SimpleMenu {
public:
  explicit SimpleMenu(std::vector<String> items) : _items(items), _index(0) {}

  void up()   { if (_items.empty()) return; _index = (_index > 0) ? _index - 1 : (int)_items.size() - 1; }
  void down() { if (_items.empty()) return; _index = (_index < (int)_items.size() - 1) ? _index + 1 : 0; }

  int index() const { return _index; }
  const std::vector<String>& items() const { return _items; }

  // Jump to an arbitrary index, wrapping around - used for 2D grid
  // navigation (e.g. the icon home screen) where up/down move by more
  // than one position.
  void setIndex(int i) {
    if (_items.empty()) return;
    int n = (int)_items.size();
    _index = ((i % n) + n) % n;
  }

  // Replace the item list at runtime (e.g. a directory listing) and reset
  // the selection back to the top.
  void setItems(std::vector<String> items) { _items = items; _index = 0; }

private:
  std::vector<String> _items;
  int _index;
};