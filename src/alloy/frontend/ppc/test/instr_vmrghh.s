test_vmrghh_1:
  #_ REGISTER_IN v3 [00010203, 04050607, 08090A0B, 0C0D0E0F]
  #_ REGISTER_IN v4 [10111213, 14151617, 18191A1B, 1C1D1E1F]
  vmrghh v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [00010203, 04050607, 08090A0B, 0C0D0E0F]
  #_ REGISTER_OUT v4 [10111213, 14151617, 18191A1B, 1C1D1E1F]
  #_ REGISTER_OUT v5 [00011011, 02031213, 04051415, 06071617]
