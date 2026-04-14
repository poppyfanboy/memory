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

class HeapIterator {
    constructor(buffer, address) {
        this.view = new Uint32Array(buffer, address);
    }

    get region() {
        return this.view[0];
    }

    get memory() {
        return this.view[1];
    }

    get size() {
        return this.view[2];
    }

    get isFree() {
        return this.view[3] != 0;
    }
}

const systemHeap = new SystemHeap(16);

const { instance } = await WebAssembly.instantiateStreaming(fetch('lib.wasm'), {
    env: {
        memory: systemHeap.memory,
        system_allocate: (_userContext, size) => systemHeap.grow(size),
    }
});

systemHeap.offset = instance.exports.__heap_base;
const allocatorAddress = instance.exports.heap_allocator();

const displayElement = document.getElementById('display');

function allocatorDump() {
    const iteratorAddress = instance.exports.heap_iterator();
    const iterator = new HeapIterator(systemHeap.memory.buffer, iteratorAddress);

    let displayText = '';
    while (true) {
        instance.exports.heap_iterate(allocatorAddress, iteratorAddress);
        if (iterator.memory == 0) {
            break;
        }

        const addressFormatted = `0x${iterator.memory.toString(16).padStart(8, '0')}`;
        const sizeFormatted = iterator.size.toString().padStart(9);
        const blockDescription = iterator.memory == allocatorAddress
            ? 'Allocator metadata'
            : `${iterator.isFree ? 'Free' : 'Occupied'} block`;

        displayText += `[${addressFormatted}] ${sizeFormatted} bytes: ${blockDescription}\n`;
    }

    displayElement.textContent = displayText;
}

const sizeInput = document.getElementById('size-input');
const allocateButton = document.getElementById('allocate-button');

allocateButton.addEventListener('click', () => {
    instance.exports.heap_allocate(allocatorAddress, sizeInput.value);
    allocatorDump();
});

allocatorDump();
