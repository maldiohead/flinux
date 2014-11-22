#include <binfmt/elf.h>
#include <common/auxvec.h>
#include <common/errno.h>
#include <common/fcntl.h>
#include <fs/winfs.h>
#include <syscall/exec.h>
#include <syscall/mm.h>
#include <syscall/process.h>
#include <syscall/tls.h>
#include <syscall/vfs.h>
#include <log.h>
#include <heap.h>

#include <Windows.h>

#ifdef _WIN64
#define Elf_Ehdr Elf64_Ehdr
#define Elf_Phdr Elf64_Phdr
#else
#define Elf_Ehdr Elf32_Ehdr
#define Elf_Phdr Elf32_Phdr
#endif

struct elf_header
{
	size_t load_base, low, high;
	Elf_Ehdr eh;
	char pht[];
};

__declspec(noreturn) void goto_entrypoint(const char *stack, void *entrypoint);

/* Macros for easier initial stack mangling */
#define PTR(ptr) *(void**)(stack -= sizeof(void*)) = (void*)(ptr)
#define AUX_VEC(id, value) PTR(value); PTR(id)
#define ALLOC(size) (stack -= (size))

static void run(struct elf_header *executable, struct elf_header *interpreter, int argc, char *argv[], int env_size, char *envp[], PCONTEXT context)
{
	/* Generate initial stack */
	char *stack_base = process_get_stack_base();
	char *stack = stack_base + STACK_SIZE;
	/* 16 random bytes for AT_RANDOM */
	/* TODO: Fill in real content */
	char *random_bytes = ALLOC(16);

	/* auxiliary vector */
	PTR(NULL);
	AUX_VEC(AT_FLAGS, 0);
	AUX_VEC(AT_SECURE, 0);
	AUX_VEC(AT_RANDOM, random_bytes);
	AUX_VEC(AT_PAGESZ, PAGE_SIZE);
	AUX_VEC(AT_PHDR, executable->pht);
	AUX_VEC(AT_PHENT, executable->eh.e_phentsize);
	AUX_VEC(AT_PHNUM, executable->eh.e_phnum);
	AUX_VEC(AT_ENTRY, executable->load_base + executable->eh.e_entry);
	AUX_VEC(AT_BASE, (interpreter ? interpreter->load_base - interpreter->low : NULL));

	/* environment variables */
	PTR(NULL);
	for (int i = env_size - 1; i >= 0; i--)
		PTR(envp[i]);

	/* argv */
	PTR(NULL);
	for (int i = argc - 1; i >= 0; i--)
		PTR(argv[i]);

	/* argc */
	/* TODO: We need to verify if this works on amd64 */
	PTR(argc);

	/* Call executable entrypoint */
	size_t entrypoint = interpreter? interpreter->load_base + interpreter->eh.e_entry: executable->load_base + executable->eh.e_entry;
	log_info("Entrypoint: %x\n", entrypoint);
	/* If we're starting from main(), just jump to entrypoint */
	if (!context)
		goto_entrypoint(stack, entrypoint);
	/* Otherwise, we're at execve() in syscall handler context */
	/* TODO: Add a trampoline to free original stack */
#ifdef _WIN64
	context->Rax = 0;
	context->Rcx = 0;
	context->Rdx = 0;
	context->Rbx = 0;
	context->Rsp = stack;
	context->Rbp = 0;
	context->Rsi = 0;
	context->Rdi = 0;
	context->Rip = entrypoint;
	context->R8 = 0;
	context->R9 = 0;
	context->R10 = 0;
	context->R11 = 0;
	context->R12 = 0;
	context->R13 = 0;
	context->R14 = 0;
	context->R15 = 0;
#else
	context->Eax = 0;
	context->Ecx = 0;
	context->Edx = 0;
	context->Ebx = 0;
	context->Esp = stack;
	context->Ebp = 0;
	context->Esi = 0;
	context->Edi = 0;
	context->Eip = entrypoint;
#endif
}

static int load_elf(const char *filename, struct elf_header **executable, struct elf_header **interpreter)
{
	Elf_Ehdr eh;
	struct file *f;
	int r = vfs_open(filename, O_RDONLY, 0, &f);
	if (r < 0)
		return r;

	if (!winfs_is_winfile(f))
		return -EACCES;

	/* Load ELF header */
	f->op_vtable->pread(f, &eh, sizeof(eh), 0);
	if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN)
	{
		log_error("Only ET_EXEC and ET_DYN executables can be loaded.\n");
		vfs_release(f);
		return -EACCES;
	}

	if (eh.e_machine != EM_386)
	{
		log_error("Not an i386 executable.\n");
		vfs_release(f);
		return -EACCES;
	}

	/* Load program header table */
	size_t phsize = (size_t)eh.e_phentsize * (size_t)eh.e_phnum;
	struct elf_header *elf = kmalloc(sizeof(struct elf_header) + phsize); /* TODO: Free it at execve */
	*executable = elf;
	if (interpreter)
		*interpreter = NULL;
	elf->eh = eh;
	f->op_vtable->pread(f, elf->pht, phsize, eh.e_phoff);

	/* Find virtual address range */
	elf->low = 0xFFFFFFFF;
	elf->high = 0;
	for (int i = 0; i < eh.e_phnum; i++)
	{
		Elf_Phdr *ph = (Elf_Phdr *)&elf->pht[eh.e_phentsize * i];
		if (ph->p_type == PT_LOAD)
		{
			elf->low = min(elf->low, ph->p_vaddr);
			elf->high = max(elf->high, ph->p_vaddr + ph->p_memsz);
			log_info("PT_LOAD: vaddr %x, size %x\n", ph->p_vaddr, ph->p_memsz);
		}
		else if (ph->p_type == PT_DYNAMIC)
			log_info("PT_DYNAMIC: vaddr %x, size %x\n", ph->p_vaddr, ph->p_memsz);
		else if (ph->p_type == PT_PHDR) /* Patch phdr pointer in PT_PHDR, glibc uses it to determine load offset */
			ph->p_vaddr = elf->pht;
	}

	/* Find virtual address range for ET_DYN executable */
	elf->load_base = 0;
	if (eh.e_type == ET_DYN)
	{
		size_t free_addr = mm_find_free_pages(elf->high - elf->low) * PAGE_SIZE;
		if (!free_addr)
		{
			vfs_release(f);
			return -ENOMEM;
		}
		elf->load_base = free_addr - elf->low;
		log_info("ET_DYN load offset: %x, real range [%x, %x)\n", elf->load_base, elf->load_base + elf->low, elf->load_base + elf->high);
	}

	/* Map executable segments */
	for (int i = 0; i < eh.e_phnum; i++)
	{
		Elf_Phdr *ph = (Elf_Phdr *)&elf->pht[eh.e_phentsize * i];
		if (ph->p_type == PT_LOAD)
		{
			size_t addr = ph->p_vaddr & 0xFFFFF000;
			size_t size = ph->p_memsz + (ph->p_vaddr & 0x00000FFF);
			off_t offset_pages = ph->p_offset / PAGE_SIZE;

			int prot = 0;
			if (ph->p_flags & PF_R)
				prot |= PROT_READ;
			if (ph->p_flags & PF_W)
				prot |= PROT_WRITE;
			if (ph->p_flags & PF_X)
				prot |= PROT_EXEC;
			mm_mmap(elf->load_base + addr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, NULL, 0);
			f->op_vtable->pread(f, (char *)(elf->load_base + ph->p_vaddr), ph->p_filesz, ph->p_offset);
			mm_update_brk((size_t)addr + size);
		}
	}

	/* Load interpreter if present */
	for (int i = 0; i < eh.e_phnum; i++)
	{
		Elf_Phdr *ph = (Elf_Phdr *)&elf->pht[eh.e_phentsize * i];
		if (ph->p_type == PT_INTERP)
		{
			if (interpreter == NULL)
			{
				vfs_release(f);
				return -EACCES; /* Bad interpreter */
			}
			char path[MAX_PATH];
			f->op_vtable->pread(f, path, ph->p_filesz, ph->p_offset);
			path[ph->p_filesz] = 0;
			if (load_elf(path, interpreter, NULL) < 0)
			{
				vfs_release(f);
				return -EACCES; /* Bad interpreter */
			}
		}
	}
	vfs_release(f);
	return 0;
}

int do_execve(const char *filename, int argc, char *argv[], int env_size, char *envp[], PCONTEXT context)
{
	struct elf_header *executable, *interpreter;
	int r = load_elf(filename, &executable, &interpreter);
	if (r < 0)
		return r;
	run(executable, interpreter, argc, argv, env_size, envp, context);
	return 0;
}

static char *const startup = (char *)STARTUP_DATA_BASE;

int sys_execve(const char *filename, char *argv[], char *envp[], int _4, int _5, int _6, PCONTEXT context)
{
	/* TODO: Deal with argv/envp == NULL */
	/* TODO: Don't destroy things on failure */
	log_info("execve(%s, %x, %x)\n", filename, argv, envp);
	log_info("Reinitializing...\n");

	/* Copy argv[] and envp[] to startup data */
	char *base = startup;
	int argc, env_size;
	for (argc = 0; argv[argc]; argc++)
	{
		base += strlen(argv[argc]) + 1;
		log_info("argv[%d] = \"%s\"\n", argc, argv[argc]);
	}
	log_info("argc = %d\n", argc);
	for (env_size = 0; envp[env_size]; env_size++)
	{
		base += strlen(envp[env_size]) + 1;
		log_info("envp[%d] = \"%s\"\n", env_size, envp[env_size]);
	}
	log_info("env_size = %d\n", env_size);

	/* TODO: Test if we have enough size to hold the startup data */
	
	char **new_argv = (char **)((uintptr_t)(base + sizeof(void*) - 1) & -sizeof(void*));
	char **new_envp = new_argv + argc + 1;

	base = startup;
	for (int i = 0; i < argc; i++)
	{
		new_argv[i] = base;
		int len = strlen(argv[i]);
		memcpy(base, argv[i], len + 1);
		base += len + 1;
	}
	new_argv[argc] = NULL;
	for (int i = 0; i < env_size; i++)
	{
		new_envp[i] = base;
		int len = strlen(envp[i]);
		memcpy(base, envp[i], len + 1);
		base += len + 1;
	}
	new_envp[env_size] = NULL;

	/* TODO: This is really ugly, we should move it into a specific UTF8->UTF16 conversion routine when we supports unicode */
	/* Normalize filename */
	char fb[1024];
	strcpy(fb, filename);
	char *f = fb;
	while (*f == ' ' || *f == '\t' || *f == '\r' || *f == '\n')
		f++;
	int len = strlen(f);
	while (f[len - 1] == ' ' || f[len - 1] == '\t' || f[len - 1] == '\r' || f[len - 1] == '\n')
		f[--len] = 0;

	vfs_reset();
	mm_reset();
	tls_reset();
	if (do_execve(f, argc, new_argv, env_size, new_envp, context) != 0)
	{
		log_warning("execve() failed.\n");
		ExitProcess(0); /* TODO: Recover */
	}
	return 0;
}
