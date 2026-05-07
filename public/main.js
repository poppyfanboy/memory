class SystemHeap {
    constructor(initialPageCount) {
        this.memory = new WebAssembly.Memory({ initial: initialPageCount });
        this.view = new Uint8Array(this.memory.buffer);

        this.pageCount = initialPageCount;
        this.pageSize = this.view.length / initialPageCount;
        this.offset = 0;
    }

    grow(increment) {
        if (this.offset + increment > this.view.length) {
            const growByBytes = (this.offset + increment) - this.view.length;
            const growByPages = Math.ceil(growByBytes / this.pageSize);

            this.memory.grow(growByPages);
            this.view = new Uint8Array(this.memory.buffer);
            this.pageCount += growByPages;
        }

        const oldOffset = this.offset;
        this.offset += increment;
        return oldOffset;
    }
}

class HeapAllocator {
    constructor(instance, systemHeap, address) {
        this.instance = instance;
        this.systemHeap = systemHeap;
        this.address = address;
    }

    static async initialize() {
        const systemHeap = new SystemHeap(16);

        const { instance } = await WebAssembly.instantiateStreaming(fetch('lib.wasm'), {
            env: {
                memory: systemHeap.memory,
                system_allocate: (_userContext, size) => systemHeap.grow(size),
            }
        });

        systemHeap.offset = instance.exports.__heap_base;
        const address = instance.exports.heap_allocator();

        return new this(instance, systemHeap, address);
    }

    allocate(size) {
        return this.instance.exports.heap_allocate(this.address, size);
    }

    deallocate(memoryAddress) {
        this.instance.exports.heap_deallocate(this.address, memoryAddress);
    }

    blocks() {
        return new HeapIterator(this);
    }
}

class HeapIterator {
    constructor(allocator) {
        this.allocator = allocator;

        const iteratorAddress = this.allocator.instance.exports.heap_iterator();
        const iteratorBuffer = this.allocator.systemHeap.memory.buffer;
        this.view = new Uint32Array(iteratorBuffer, iteratorAddress);

        this.allocator.instance.exports.heap_iterate(allocator.address, iteratorAddress);
    }

    next() {
        const value = {
            region: this.view[0],
            memory: this.view[1],
            size: this.view[2],
            isFree: this.view[3] != 0,
        };
        if (value.memory == 0) {
            return { done: true };
        }

        this.allocator.instance.exports.heap_iterate(this.allocator.address, this.view.byteOffset);
        return { value, done: false };
    }

    [Symbol.iterator]() {
        return this;
    }
}

class BlockList {
    constructor(allocator, root) {
        this.allocator = allocator;
        this.root = root;

        this.root.addEventListener('click', (event) => {
            const deleteButton = event.target.closest('button');

            if (deleteButton != null && deleteButton.dataset.address != null) {
                const memoryAddress = Number.parseInt(deleteButton.dataset.address);
                this.allocator.deallocate(memoryAddress);
                this.refresh();
            }
        });

        this.refresh();
    }

    refresh() {
        while (this.root.firstChild != null) {
            this.root.firstChild.remove();
        }

        const listElementTemplate = document.getElementById('block-list-element-template');

        for (const block of this.allocator.blocks()) {
            const listElementNode = document.importNode(listElementTemplate.content, true);

            const addressElement = listElementNode.querySelector('.block-list-element__address');
            const labelElement = listElementNode.querySelector('.block-list-element__label');
            const sizeElement = listElementNode.querySelector('.block-list-element__size');

            addressElement.textContent = `0x${block.memory.toString(16).padStart(8, '0')}`;
            sizeElement.textContent = `Size: ${block.size} B`;
            labelElement.textContent = block.memory == this.allocator.address
                ? 'Allocator metadata'
                : `${block.isFree ? 'Free' : 'Occupied'} block`;

            if (block.memory == this.allocator.address || block.isFree) {
                const listElement = listElementNode.querySelector('.block-list-element');
                listElement.classList.add('block-list-element--disabled');
            }

            const deleteButton = listElementNode.querySelector(
                '.block-list-element__delete-button'
            );
            deleteButton.dataset.address = block.memory.toString();

            this.root.appendChild(listElementNode);
        }
    }
}

const themeButton = document.getElementById('theme-button');

themeButton.addEventListener('click', () => {
    let theme = document.documentElement.dataset.theme ?? 'light';

    if (theme == 'light') {
        theme = 'dark';
    } else {
        theme = 'light';
    }

    localStorage.setItem('theme', theme);
    document.documentElement.dataset.theme = theme;
});

const allocator = await HeapAllocator.initialize();

const blockList = new BlockList(allocator, document.getElementById('block-list'));
const allocationSizeInput = document.getElementById('allocation-size-input');
const allocateButton = document.getElementById('allocate-button');

allocateButton.addEventListener('click', () => {
    allocator.allocate(allocationSizeInput.value);
    blockList.refresh();
});
