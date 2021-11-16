#include <llvm/IR/DataLayout.h>

#include "llvm.h"

namespace llvm {

bool getelementptr_can_zero(const llvm::GetElementPtrInst *GEP) {
    for (const llvm::Value *V : GEP->indices()) {
        if (const llvm::ConstantInt *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
            if (CI->getValue().getLimitedValue() != 0) {
                return false;
            }
        }
    }
    return true;
}

std::optional<int> getelementptr_const_offset(const llvm::GetElementPtrInst *GEP) {
    const llvm::Module *M = GEP->getParent()->getParent()->getParent();
    llvm::DataLayout layout {M};
    
    llvm::Type *T = GEP->getPointerOperandType();
    std::size_t offset = 0;
    for (const llvm::Value *V : GEP->indices()) {
        if (const llvm::ConstantInt *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
            const uint64_t index = CI->getSExtValue();
            
            if (const llvm::StructType *ST = llvm::dyn_cast<llvm::StructType>(T)) {
                for (uint64_t i = 0; i < index; ++i) {
                    offset += layout.getTypeSizeInBits(ST->getElementType(i));
                }
                T = ST->getElementType(index);
            } else if (const llvm::ArrayType *AT = llvm::dyn_cast<llvm::ArrayType>(T)) {
                T = AT->getElementType();
                offset += layout.getTypeSizeInBits(T) * index;
            } else if (const llvm::PointerType *PT = llvm::dyn_cast<llvm::PointerType>(T)) {
                T = PT->getElementType();
                offset += layout.getTypeSizeInBits(T) * index;
            } else {
                std::abort();
            }
        } else {
            return std::nullopt;
        }
    }
    
    return offset;
}

bool contains_struct(const llvm::Type *T) {
    if (const auto *AT = llvm::dyn_cast<llvm::ArrayType>(T)) {
        return contains_struct(AT->getElementType());
    } else if (const auto *FT = llvm::dyn_cast<llvm::FunctionType>(T)) {
        return false;
    } else if (const auto *IT = llvm::dyn_cast<llvm::IntegerType>(T)) {
        return false;
    } else if (const auto *PT = llvm::dyn_cast<llvm::PointerType>(T)) {
        return false;
    } else if (const auto *ST = llvm::dyn_cast<llvm::StructType>(T)) {
        return true;
    } else if (const auto *VT = llvm::dyn_cast<llvm::VectorType>(T)) {
        return contains_struct(VT->getElementType());
    } else {
        std::abort();
    }
}

}
