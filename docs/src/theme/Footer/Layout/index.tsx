import React, {type ReactNode} from 'react';
import clsx from 'clsx';
import {ThemeClassNames} from '@docusaurus/theme-common';
import type {Props} from '@theme/Footer/Layout';

export default function FooterLayout({
  style,
  links,
  logo,
  copyright,
}: Props): ReactNode {
  return (
    <footer
      className={clsx(ThemeClassNames.layout.footer.container, 'footer', {
        'footer--dark': style === 'dark',
      })}>
      <div className="container container-fluid">
        {links}
        {(logo || copyright) && (
          <div className="footer__bottom" style={{
            display: 'flex',
            flexDirection: 'row',
            justifyContent: 'space-between',
            alignItems: 'center',
            marginTop: '2.5rem',
            paddingTop: '1.5rem',
            borderTop: '1px solid var(--ifm-color-emphasis-300)',
            fontSize: '0.75rem',
            color: 'var(--ifm-footer-link-color)',
            flexWrap: 'wrap',
            gap: '1rem'
          }}>
            <div style={{ margin: 0 }}>
              {copyright}
            </div>
            <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem' }}>
              <img src="/logos/nsf.png" alt="NSF" style={{ height: '1.5rem' }} />
              <span>NSF CSSI-2104013</span>
            </div>
          </div>
        )}
      </div>
    </footer>
  );
}
