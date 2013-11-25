set(LLVM_LINK_COMPONENTS
  asmparser
  bitreader
  bitwriter
  codegen
  instrumentation
  ipo
  irreader
  linker
  selectiondag
  )

add_llvm_tool(le32-none-ndk-strip
  le32-none-ndk-strip.cpp
  )
