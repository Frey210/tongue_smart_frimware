#pragma once
#include "types.h"

class StorageManager {
 public:
  bool begin();
  bool saveResult(const ExaminationResult& result);
  void listResults(Stream& output);
  bool clearResults();
};

