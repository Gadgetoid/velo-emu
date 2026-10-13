#!/usr/bin/env python3
import struct
import sys

ENTRY_SIZE = 32
ATTRIBUTE_LONG_NAME = 0x0F
ATTRIBUTE_VOLUME = 0x08
ATTRIBUTE_DIRECTORY = 0x10
CASE_LOWER_BASE = 0x08
CASE_LOWER_EXTENSION = 0x10
DELETED = 0xE5
END_OF_CHAIN = 0xFFF8
LONG_NAME_CHARACTERS = 13
LONG_NAME_OFFSETS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)


class FatImage:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as file:
            self.data = bytearray(file.read())
        self.base = self.find_volume()
        boot = self.data[self.base:self.base + 512]
        (self.bytes_per_sector, self.sectors_per_cluster, reserved, self.fat_count, self.root_entries,
         _, _, self.fat_sectors) = struct.unpack_from("<HBHBHHBH", boot, 11)
        if self.fat_sectors == 0 or boot[54:59] != b"FAT16":
            raise SystemExit(f"fatcase: {path} isn't a FAT16 volume")
        self.fat_start = self.base + reserved * self.bytes_per_sector
        self.root_start = self.fat_start + self.fat_count * self.fat_sectors * self.bytes_per_sector
        self.data_start = self.root_start + self.root_entries * ENTRY_SIZE
        self.cluster_size = self.sectors_per_cluster * self.bytes_per_sector
        self.cluster_count = (len(self.data) - self.data_start) // self.cluster_size + 2
        self.fat_entries = self.fat_sectors * self.bytes_per_sector // 2

    def find_volume(self):
        if self.data[54:59] == b"FAT16":
            return 0
        if self.data[510:512] != b"\x55\xaa":
            raise SystemExit(f"fatcase: {self.path} has no boot sector or partition table")
        first_sector = struct.unpack_from("<I", self.data, 446 + 8)[0]
        return first_sector * 512

    def fat(self, cluster):
        return struct.unpack_from("<H", self.data, self.fat_start + cluster * 2)[0]

    def set_fat(self, cluster, value):
        for copy in range(self.fat_count):
            struct.pack_into("<H", self.data, self.fat_start + copy * self.fat_sectors * self.bytes_per_sector + cluster * 2, value)

    def chain(self, first):
        clusters = []
        cluster = first
        while 2 <= cluster < END_OF_CHAIN and len(clusters) < self.cluster_count:
            clusters.append(cluster)
            cluster = self.fat(cluster)
        return clusters

    def cluster_offset(self, cluster):
        return self.data_start + (cluster - 2) * self.cluster_size

    def allocate_after(self, last):
        for cluster in range(2, min(self.cluster_count, self.fat_entries)):
            if self.fat(cluster) == 0:
                self.set_fat(last, cluster)
                self.set_fat(cluster, 0xFFFF)
                offset = self.cluster_offset(cluster)
                self.data[offset:offset + self.cluster_size] = bytes(self.cluster_size)
                return cluster
        raise SystemExit(f"fatcase: {self.path} is full")

    def save(self):
        with open(self.path, "wb") as file:
            file.write(self.data)


def short_name_checksum(short_name):
    checksum = 0
    for byte in short_name:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 0xFF
    return checksum


def long_name_for(entry):
    base = entry[0:8].decode("latin-1").rstrip()
    extension = entry[8:11].decode("latin-1").rstrip()
    if entry[12] & CASE_LOWER_BASE:
        base = base.lower()
    if entry[12] & CASE_LOWER_EXTENSION:
        extension = extension.lower()
    return base + ("." + extension if extension else "")


def long_name_entries(name, short_name):
    characters = [ord(character) for character in name] + [0]
    pieces = (len(characters) + LONG_NAME_CHARACTERS - 1) // LONG_NAME_CHARACTERS
    characters += [0xFFFF] * (pieces * LONG_NAME_CHARACTERS - len(characters))
    checksum = short_name_checksum(short_name)
    entries = []
    for index in range(pieces):
        entry = bytearray(ENTRY_SIZE)
        entry[0] = (index + 1) | (0x40 if index == pieces - 1 else 0)
        entry[11] = ATTRIBUTE_LONG_NAME
        entry[13] = checksum
        for position, offset in enumerate(LONG_NAME_OFFSETS):
            struct.pack_into("<H", entry, offset, characters[index * LONG_NAME_CHARACTERS + position])
        entries.append(bytes(entry))
    return list(reversed(entries))


def rewrite_entries(entries):
    rewritten = []
    subdirectories = []
    has_long_name = False
    for entry in entries:
        if entry[0] == 0:
            break
        if entry[0] == DELETED:
            has_long_name = False
            continue
        if entry[11] == ATTRIBUTE_LONG_NAME:
            rewritten.append(entry)
            has_long_name = True
            continue
        entry = bytearray(entry)
        is_dot = entry[0] == ord(".")
        if not is_dot and not entry[11] & ATTRIBUTE_VOLUME and entry[12] & (CASE_LOWER_BASE | CASE_LOWER_EXTENSION) and not has_long_name:
            rewritten.extend(long_name_entries(long_name_for(entry), bytes(entry[0:11])))
            entry[12] &= ~(CASE_LOWER_BASE | CASE_LOWER_EXTENSION) & 0xFF
        if entry[11] & ATTRIBUTE_DIRECTORY and not is_dot and not entry[11] & ATTRIBUTE_VOLUME:
            subdirectories.append(struct.unpack_from("<H", entry, 26)[0])
        rewritten.append(bytes(entry))
        has_long_name = False
    return rewritten, subdirectories


def read_entries(image, offset, size):
    return [bytes(image.data[offset + index:offset + index + ENTRY_SIZE]) for index in range(0, size, ENTRY_SIZE)]


def write_entries(image, offset, size, entries):
    block = b"".join(entries)
    image.data[offset:offset + size] = block + bytes(size - len(block))


def fix_root(image):
    size = image.root_entries * ENTRY_SIZE
    entries, subdirectories = rewrite_entries(read_entries(image, image.root_start, size))
    if len(entries) > image.root_entries:
        raise SystemExit(f"fatcase: the root folder of {image.path} has no room for long names")
    write_entries(image, image.root_start, size, entries)
    return subdirectories


def fix_directory(image, first_cluster):
    clusters = image.chain(first_cluster)
    entries = []
    for cluster in clusters:
        entries += read_entries(image, image.cluster_offset(cluster), image.cluster_size)
    entries, subdirectories = rewrite_entries(entries)
    per_cluster = image.cluster_size // ENTRY_SIZE
    while len(clusters) * per_cluster <= len(entries):
        clusters.append(image.allocate_after(clusters[-1]))
    for index, cluster in enumerate(clusters):
        write_entries(image, image.cluster_offset(cluster), image.cluster_size, entries[index * per_cluster:(index + 1) * per_cluster])
    return subdirectories


def main():
    if len(sys.argv) != 2:
        print("usage: fatcase.py IMAGE", file=sys.stderr)
        return 2
    image = FatImage(sys.argv[1])
    pending = fix_root(image)
    visited = set()
    while pending:
        cluster = pending.pop()
        if cluster in visited or cluster < 2:
            continue
        visited.add(cluster)
        pending += fix_directory(image, cluster)
    image.save()
    return 0


if __name__ == "__main__":
    sys.exit(main())
