// Copyright (c) 2013, Facebook, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name Facebook nor the names of its contributors may be used to
//     endorse or promote products derived from this software without specific
//     prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "fishhook.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#ifdef __LP64__
typedef struct mach_header_64 mach_header_t;
typedef struct segment_command_64 segment_command_t;
typedef struct section_64 section_t;
typedef struct nlist_64 nlist_t;
//LC_SEGMENT_64:一种command类型表示将文件的64位的段映射到进程地址空间
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT_64
#else
typedef struct mach_header mach_header_t;
typedef struct segment_command segment_command_t;
typedef struct section section_t;
typedef struct nlist nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT
#endif

#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST  "__DATA_CONST"
#endif

struct rebindings_entry {
  struct rebinding *rebindings;
  size_t rebindings_nel;
  struct rebindings_entry *next;
};

static struct rebindings_entry *_rebindings_head;
// 绑定的前置动作，将rebindings放到私有的rebindings_header头部
static int prepend_rebindings(struct rebindings_entry **rebindings_head,
                              struct rebinding rebindings[],
                              size_t nel) {
  // 创建rebindings_entry节点
  struct rebindings_entry *new_entry = (struct rebindings_entry *) malloc(sizeof(struct rebindings_entry));
  if (!new_entry) {
    return -1;
  }
  new_entry->rebindings = (struct rebinding *) malloc(sizeof(struct rebinding) * nel);
  if (!new_entry->rebindings) {
    free(new_entry);
    return -1;
  }
  memcpy(new_entry->rebindings, rebindings, sizeof(struct rebinding) * nel);
  new_entry->rebindings_nel = nel;
  // 每次都将新创建的rebindings_entry放到链表rebindings_head最前面
  new_entry->next = *rebindings_head;
  *rebindings_head = new_entry;
  return 0;
}

static vm_prot_t get_protection(void *sectionStart) {
  mach_port_t task = mach_task_self();
  vm_size_t size = 0;
  vm_address_t address = (vm_address_t)sectionStart;
  memory_object_name_t object;
#if __LP64__
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  vm_region_basic_info_data_64_t info;
  kern_return_t info_ret = vm_region_64(
      task, &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_64_t)&info, &count, &object);
#else
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT;
  vm_region_basic_info_data_t info;
  kern_return_t info_ret = vm_region(task, &address, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &count, &object);
#endif
  if (info_ret == KERN_SUCCESS) {
    return info.protection;
  } else {
    return VM_PROT_READ;
  }
}

// 根据传入的nl_symbol_ptr/la_symbol_ptr数据段，遍历该数据段的符号，找到其对应的符号名并与传入的符号名进行匹配，命中则进行替换。
static void perform_rebinding_with_section(struct rebindings_entry *rebindings,
                                           section_t *section,
                                           intptr_t slide,
                                           nlist_t *symtab, // 符号表
                                           char *strtab, // 字符串表
                                           uint32_t *indirect_symtab // 动态符号表
                                           ) {
  const bool isDataConst = strcmp(section->segname, "__DATA_CONST") == 0;
  // 符号表访问指针地址替换
  // `nl_symbol_ptr`和`la_symbol_ptr`section中的`reserved1`字段指明对应在`indirect symbol table`起始的index
  //  获得该section符号表的起始地址
  uint32_t *indirect_symbol_indices = indirect_symtab + section->reserved1;
  // 得到该section段的所有函数实现地址
  void **indirect_symbol_bindings = (void **)((uintptr_t)slide + section->addr);
  vm_prot_t oldProtection = VM_PROT_READ;
  if (isDataConst) {
    oldProtection = get_protection(rebindings);
    // protect()函数可以用来修改一段指定内存区域的保护属性。
    // 这里暂时将常量区权限改成可读可写
    mprotect(indirect_symbol_bindings, section->size, PROT_READ | PROT_WRITE);
  }
  for (uint i = 0; i < section->size / sizeof(void *); i++) {
    // 从符号表中取得符号
    uint32_t symtab_index = indirect_symbol_indices[i];
    if (symtab_index == INDIRECT_SYMBOL_ABS || symtab_index == INDIRECT_SYMBOL_LOCAL ||
        symtab_index == (INDIRECT_SYMBOL_LOCAL   | INDIRECT_SYMBOL_ABS)) {
      continue;
    }
    // 获取每一个需要动态解析的符号在符号表中的偏移量
    uint32_t strtab_offset = symtab[symtab_index].n_un.n_strx;
    // 通过字符串表偏移量获取符号对应的字符串（符号的名字）
    char *symbol_name = strtab + strtab_offset;
    bool symbol_name_longer_than_1 = symbol_name[0] && symbol_name[1];
    // 遍历rebindings数组，比较符号，相同则进行替换
    struct rebindings_entry *cur = rebindings;
    while (cur) {
      for (uint j = 0; j < cur->rebindings_nel; j++) {
        if (symbol_name_longer_than_1 &&
            strcmp(&symbol_name[1], cur->rebindings[j].name) == 0) {
          // 判断原实现是否有被保存过，既实现和现在表中的实现是否一致
          if (cur->rebindings[j].replaced != NULL &&
              indirect_symbol_bindings[i] != cur->rebindings[j].replacement) {
            *(cur->rebindings[j].replaced) = indirect_symbol_bindings[i];
          }
          // 更改函数为新的实现
          indirect_symbol_bindings[i] = cur->rebindings[j].replacement;
          goto symbol_loop;
        }
      }
      cur = cur->next;
    }
  symbol_loop:;
  }
  // 恢复常量区的访问权限
  if (isDataConst) {
    int protection = 0;
    if (oldProtection & VM_PROT_READ) {
      protection |= PROT_READ;  // 按位或后赋值
    }
    if (oldProtection & VM_PROT_WRITE) {
      protection |= PROT_WRITE;
    }
    if (oldProtection & VM_PROT_EXECUTE) {
      protection |= PROT_EXEC;
    }
    mprotect(indirect_symbol_bindings, section->size, protection);
  }
}

// 计算基址；找到符号表、字符串表、间接符号表；找到nl_symbol_ptr(got)/la_symbol_ptr
/*
 为什么要找nl_symbol_ptr(got)/la_symbol_ptr？
 __nl_symbol_ptr    非懒加载指针表
 __la_symbol_ptr    懒加载指针表，符号第一次调用时通过 dyld 中的 dyld_stub_binder进行加载到表中
 这两个表是_DATA中跟动态符号链接相关的部分，所以需要找到原方法这两个部分的指针去替换链接方法
 对于动态链接库里面的C函数，第一次调用的时候，我们会得到函数和实现地址的对应关系，函数的实现地址存放在一个叫la_symbol_ptr的地方，第二次调用的时候，直接通过la_symbol_ptr找到函数地址就可以，不再需要繁琐的获取函数地址的过程。
 */
static void rebind_symbols_for_image(struct rebindings_entry *rebindings,
                                     const struct mach_header *header,
                                     intptr_t slide) {
  Dl_info info;
  if (dladdr(header, &info) == 0) {
    return;
  }
  // 找到符号表相关的 command，包括 linkedit segment command、symtab command 和 dysymtab command
  segment_command_t *cur_seg_cmd;
  segment_command_t *linkedit_segment = NULL; //LINKEDIT
  struct symtab_command* symtab_cmd = NULL; //符号表
  struct dysymtab_command* dysymtab_cmd = NULL; //间接符号表
  //1. 遍历加载命令，获得MachO中LINKEDIT、符号表、间接符号表三个加载命令
  // 每个mach-o由(header、load commands、 data)三块区域组成
  // 要去寻找load command，所以这里先跳过sizeof(mach_header_t)大小
  uintptr_t cur = (uintptr_t)header + sizeof(mach_header_t);
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
    cur_seg_cmd = (segment_command_t *)cur;
  //__LINKEDIT段 含有为动态链接库使用的原始数据，比如符号，字符串，重定位表条目等等
  /*
   LC_SEGMENT_64 含有为动态链接库使用的原始数据
   LC_SYMTAB(符号地址)这个LoadCommand主要提供了两个信息
      Symbol Table(符号表)的偏移量与Symbol Table中元素的个数
      String Table(字符串表)的偏移量与String Table的长度
   LC_DYSYMTAB(动态符号表地址)提供了动态符号表的位移和元素个数，还有一些其他的表格索引
   */
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
      if (strcmp(cur_seg_cmd->segname, SEG_LINKEDIT) == 0) {
        linkedit_segment = cur_seg_cmd;
      }
    } else if (cur_seg_cmd->cmd == LC_SYMTAB) {
      symtab_cmd = (struct symtab_command*)cur_seg_cmd;
    } else if (cur_seg_cmd->cmd == LC_DYSYMTAB) {
      dysymtab_cmd = (struct dysymtab_command*)cur_seg_cmd;
    }
  }
    
  if (!symtab_cmd || !dysymtab_cmd || !linkedit_segment ||
      !dysymtab_cmd->nindirectsyms) {
    return;
  }

  // 找到 base 符号表的地址
  // 本来是：基址=linkedit内存地址 - linkedit的fileoff
  // 由于ASLR：真实基址 = linkedit内存地址(vmaddr) + slide - fileoff
  uintptr_t linkedit_base = (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
  //符号表的地址 = 基址 + 符号表偏移量
  nlist_t *symtab = (nlist_t *)(linkedit_base + symtab_cmd->symoff);
  //字符串表的地址 = 基址 + 字符串表偏移量
  char *strtab = (char *)(linkedit_base + symtab_cmd->stroff);
  //动态符号表地址 = 基址 + 动态符号表偏移量
  uint32_t *indirect_symtab = (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);
    
  //2. 遍历加载命令，得到DATA，然后遍历DATA里面的section，找到nl_symbol_ptr(got)/la_symbol_ptr
  cur = (uintptr_t)header + sizeof(mach_header_t);
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
    cur_seg_cmd = (segment_command_t *)cur;
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
      //寻找__DATA和__DATA_CONST的section
      if (strcmp(cur_seg_cmd->segname, SEG_DATA) != 0 &&
          strcmp(cur_seg_cmd->segname, SEG_DATA_CONST) != 0) {
        continue;
      }
      //遍历DATA里面的section,找到nl_symbol_ptr(got)/la_symbol_ptr
      for (uint j = 0; j < cur_seg_cmd->nsects; j++) {
         //_DATA 加上结构体偏移
         
         //
         //          struct segment_command_64 { /* for 64-bit architectures */
          //          uint32_t    cmd;        /* LC_SEGMENT_64 */
          //          uint32_t    cmdsize;    /* includes sizeof section_64 structs*/
          //          char        segname[16];    /* segment name */
          //          uint64_t    vmaddr;        /* memory address of this segment*/
          //          uint64_t    vmsize;        /* memory size of this segment */
          //          uint64_t    fileoff;    /* file offset of this segment */
          //          uint64_t    filesize;    /* amount to map from the file */
          //          vm_prot_t    maxprot;    /* maximum VM protection */
          //          vm_prot_t    initprot;    /* initial VM protection */
          //          uint32_t    nsects;        /* number of sections in segment*/
          //          uint32_t    flags;        /* flags */
          //      };
    
         
        section_t *sect =
          (section_t *)(cur + sizeof(segment_command_t)) + j;
        //寻找__la_symbol_ptr区
        if ((sect->flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS) {
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
        //寻找__nl_symbol_ptr
        if ((sect->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
      }
    }
  }
}

static void _rebind_symbols_for_image(const struct mach_header *header,
                                      intptr_t slide) {
    rebind_symbols_for_image(_rebindings_head, header, slide);
}

int rebind_symbols_image(void *header,
                         intptr_t slide,
                         struct rebinding rebindings[],
                         size_t rebindings_nel) {
    struct rebindings_entry *rebindings_head = NULL;
    int retval = prepend_rebindings(&rebindings_head, rebindings, rebindings_nel);
    rebind_symbols_for_image(rebindings_head, (const struct mach_header *) header, slide);
    if (rebindings_head) {
      free(rebindings_head->rebindings);
    }
    free(rebindings_head);
    return retval;
}


int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel) {
  // 调用 prepend_rebindings 的函数，将整个 rebindings 数组添加到 _rebindings_head 这个私有链表的头部
  int retval = prepend_rebindings(&_rebindings_head, rebindings, rebindings_nel);
  if (retval < 0) {
    return retval;
  }
  // 判断 _rebindings_head->next 的值来判断是否为第一次调用
  if (!_rebindings_head->next) {
    // 第一次调用，则注册自定义的回调函数，同时也会为所有已经加载的动态库或可执行文件执行回调
    _dyld_register_func_for_add_image(_rebind_symbols_for_image);
  } else {
    // 非第一次调用则说明已经注册郭了_rebind_symbols_for_image，则让所有imaged重新调用该方法
    uint32_t c = _dyld_image_count();
    // 遍历所有image
    for (uint32_t i = 0; i < c; i++) {
        // 传入image header和地址偏移值slider(ASLR)
      _rebind_symbols_for_image(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
    }
  }
  return retval;
}
