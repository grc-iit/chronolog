import React, { Component } from 'react';
import OriginalDocSidebarNavbarItem from '@theme-original/NavbarItem/DocSidebarNavbarItem';
import { useLocation } from '@docusaurus/router';
import type { Props } from '@theme/NavbarItem/DocSidebarNavbarItem';

// Sidebars exclusive to version 2.4.0 (served under /docs/2.4.0/)
const V24_ONLY_SIDEBARS = new Set([
  'architectureSidebar',
  'clientSidebar',
  'forDevelopersSidebar',
]);
// Sidebars exclusive to version 2.5.0 (served at /docs/, no version prefix)
const V25_ONLY_SIDEBARS = new Set([
  'userGuideSidebar',
  'clientApiSidebar',
  'contributingSidebar',
]);

/**
 * Silently catches any render error from the original DocSidebarNavbarItem
 * (e.g. "Can't find sidebar X in version Y") and returns null instead of
 * propagating to Docusaurus's error boundary, which would crash the page.
 */
class SilentBoundary extends Component<
  { children: React.ReactNode },
  { failed: boolean }
> {
  state = { failed: false };

  static getDerivedStateFromError(): { failed: boolean } {
    return { failed: true };
  }

  // Suppress the console error that React logs for caught errors
  componentDidCatch(): void {}

  render(): React.ReactNode {
    return this.state.failed ? null : this.props.children;
  }
}

export default function DocSidebarNavbarItem(props: Props): JSX.Element | null {
  const { pathname } = useLocation();
  // 2.4.0 docs are served under /docs/2.4.0/ (routeBasePath="docs", version path="2.4.0")
  const isV24 = pathname.startsWith('/docs/2.4.0/') || pathname === '/docs/2.4.0';

  if (V24_ONLY_SIDEBARS.has(props.sidebarId) && !isV24) return null;
  if (V25_ONLY_SIDEBARS.has(props.sidebarId) && isV24) return null;

  // SilentBoundary is a defense-in-depth: if useLayoutDocsSidebar inside the
  // original component throws (e.g. version context mismatch), catch it here
  // and render nothing instead of crashing the whole navbar.
  return (
    <SilentBoundary key={`${props.sidebarId}-${isV24}`}>
      <OriginalDocSidebarNavbarItem {...props} />
    </SilentBoundary>
  );
}
