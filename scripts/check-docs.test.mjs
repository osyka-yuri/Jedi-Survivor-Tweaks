import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { headingAnchors, localReferences, markdownWithoutFencedCode } from './check-docs.mjs';

describe('documentation Markdown parsing', () => {
  it('ignores Markdown and HTML references inside fenced code', () => {
    const markdown = `
[real link](guide.md#start)

\`\`\`markdown
[example link](missing.md)
![example image](missing.webp)
<a href="also-missing.md">example</a>
\`\`\`

~~~html
<img src="missing-again.webp" alt="Example">
~~~

![real image](screenshot.webp)
`;

    assert.deepEqual(localReferences(markdown), [
      { destination: 'guide.md#start', isImage: false, alt: 'real link' },
      { destination: 'screenshot.webp', isImage: true, alt: 'real image' },
    ]);
  });

  it('recognizes centered-header links and images', () => {
    const markdown = `
<div align="center">
  <a href="docs/README.md"><img src="badge.svg" alt="Documentation"></a>
</div>
`;

    assert.deepEqual(localReferences(markdown), [
      { destination: 'docs/README.md', isImage: false, alt: undefined },
      { destination: 'badge.svg', isImage: true, alt: 'Documentation' },
    ]);
  });

  it('recognizes longer closing fences and preserves surrounding prose', () => {
    const markdown = `before
\`\`\`\`text
hidden
\`\`\`\`\`
after`;

    assert.equal(markdownWithoutFencedCode(markdown), 'before\n\n\n\nafter');
  });

  it('builds deterministic GitHub-style anchors outside fenced examples', () => {
    const markdown = `
## Safety and compatibility
## Safety and compatibility

\`\`\`
## Not a real heading
\`\`\`

## Français et 日本語
`;

    assert.deepEqual(
      [...headingAnchors(markdown)],
      ['safety-and-compatibility', 'safety-and-compatibility-1', 'français-et-日本語'],
    );
  });
});
