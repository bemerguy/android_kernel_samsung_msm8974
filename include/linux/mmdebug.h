#ifndef LINUX_MM_DEBUG_H
#define LINUX_MM_DEBUG_H 1

#ifdef CONFIG_DEBUG_VM
#define VM_BUG_ON(cond) BUG_ON(cond)
#else
#define VM_BUG_ON(cond) do { (void)(cond); } while (0)
#endif

#define VM_BUG_ON_PAGE(cond, page)                                      \
        do {                                                            \
                if (unlikely(cond)) {                                   \
                        dump_page(page);\
                        BUG();                                          \
                }                                                       \
        } while (0)

#ifdef CONFIG_DEBUG_VIRTUAL
#define VIRTUAL_BUG_ON(cond) BUG_ON(cond)
#else
#define VIRTUAL_BUG_ON(cond) do { } while (0)
#endif

#endif
