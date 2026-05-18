import React, {type ReactNode} from 'react';

import {isMultiColumnFooterLinks} from '@docusaurus/theme-common';
import FooterLinksMultiColumn from '@theme/Footer/Links/MultiColumn';
import FooterLinksSimple from '@theme/Footer/Links/Simple';
import type {Props} from '@theme/Footer/Links';

export default function FooterLinks({links}: Props): ReactNode {
  // Our custom footer uses objects with 'items' for all columns, 
  // but the first column intentionally omits a 'title'.
  // We bypass the default 'isMultiColumnFooterLinks' check to force MultiColumn.
  return <FooterLinksMultiColumn columns={links as any} />;
}
