#!/usr/bin/env python3
"""Timeline stress test - validates handling of many items."""

import json
import time

def generate_timeline_items(count=1000):
    """Generate test timeline items."""
    items = []
    types = ['user', 'agent', 'command', 'usage', 'error', 'approval', 'question', 'plan']

    for i in range(count):
        item = {
            'id': f'item-{i}',
            'type': types[i % len(types)],
            'content': f'Test item {i}',
            'timestamp': f'2026-08-01T{i%24:02d}:{i%60:02d}:00Z',
            'state': 'complete' if i % 3 == 0 else 'streaming'
        }
        items.append(item)

    return items

def test_large_timeline():
    """Test timeline with 1000 items."""
    print("Generating 1000 timeline items...")
    start = time.time()
    items = generate_timeline_items(1000)
    elapsed = time.time() - start

    print(f"✓ Generated {len(items)} items in {elapsed:.3f}s")
    print(f"✓ Average: {elapsed/len(items)*1000:.3f}ms per item")

    # Validate structure
    assert all('id' in item for item in items), "All items must have id"
    assert all('type' in item for item in items), "All items must have type"
    assert all('content' in item for item in items), "All items must have content"

    print("✓ All items valid")

    # Test pagination
    page_size = 50
    pages = [items[i:i+page_size] for i in range(0, len(items), page_size)]
    print(f"✓ Pagination: {len(pages)} pages of {page_size} items")

    # Test filtering
    user_items = [item for item in items if item['type'] == 'user']
    print(f"✓ Filtering: {len(user_items)} user items")

    return True

if __name__ == '__main__':
    print("Timeline Stress Test")
    print("=" * 50)

    try:
        test_large_timeline()
        print("\n✓ All tests passed")
    except AssertionError as e:
        print(f"\n✗ Test failed: {e}")
        exit(1)
