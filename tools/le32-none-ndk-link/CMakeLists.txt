set(LLVM_LINK_COMPONENTS
  asmparser
  bitreader
  bitwriter
  codegen
  instrumentation
  ipo
  linker
  selectiondag
  )

add_llvm_tool(le32-none-ndk-link
  AndroidBitcodeLinker.cpp
  le32-none-ndk-link.cpp
  )
